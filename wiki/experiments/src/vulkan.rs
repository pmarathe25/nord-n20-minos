//! Synchronous Vulkan compute with resident buffers and reusable pipelines.

use std::cell::Cell;
use std::ffi::CStr;
use std::io::Cursor;
use std::marker::PhantomData;
use std::ptr::{self, NonNull};
use std::time::{Duration, Instant};

use ash::{Entry, vk};

use crate::Result;

pub struct Gpu {
    _entry: Entry,
    instance: ash::Instance,
    device: ash::Device,
    queue: vk::Queue,
    pool: vk::CommandPool,
    memory: vk::PhysicalDeviceMemoryProperties,
    props: vk::PhysicalDeviceProperties,
    // The queue, command pool, and mapped buffers use one host thread.
    _single_thread: PhantomData<Cell<()>>,
}

impl Gpu {
    pub fn open_adreno() -> Result<Self> {
        let entry = unsafe { Entry::load()? };
        let app = vk::ApplicationInfo::default().api_version(vk::API_VERSION_1_1);
        let info = vk::InstanceCreateInfo::default().application_info(&app);
        let instance = unsafe { entry.create_instance(&info, None)? };
        let opened = Self::open_device(&instance);
        let (device, physical, family, props, pool) = match opened {
            Ok(opened) => opened,
            Err(error) => {
                unsafe { instance.destroy_instance(None) };
                return Err(error);
            }
        };
        let memory = unsafe { instance.get_physical_device_memory_properties(physical) };
        let queue = unsafe { device.get_device_queue(family, 0) };
        Ok(Self {
            _entry: entry,
            instance,
            device,
            queue,
            pool,
            memory,
            props,
            _single_thread: PhantomData,
        })
    }

    fn open_device(instance: &ash::Instance) -> Result<OpenedDevice> {
        for physical in unsafe { instance.enumerate_physical_devices()? } {
            let props = unsafe { instance.get_physical_device_properties(physical) };
            if props.vendor_id != 0x5143
                || props.device_type != vk::PhysicalDeviceType::INTEGRATED_GPU
                || props.api_version < vk::API_VERSION_1_1
            {
                continue;
            }
            let mut subgroup = vk::PhysicalDeviceSubgroupProperties::default();
            let mut query = vk::PhysicalDeviceProperties2::default().push_next(&mut subgroup);
            unsafe { instance.get_physical_device_properties2(physical, &mut query) };
            let required = vk::SubgroupFeatureFlags::SHUFFLE | vk::SubgroupFeatureFlags::ARITHMETIC;
            if !subgroup
                .supported_stages
                .contains(vk::ShaderStageFlags::COMPUTE)
                || !subgroup.supported_operations.contains(required)
                || !matches!(subgroup.subgroup_size, 16 | 32 | 64 | 128)
                || props.limits.max_compute_work_group_invocations < 128
                || props.limits.max_compute_work_group_size[0] < 128
                || props.limits.max_compute_shared_memory_size < 8256
            {
                continue;
            }
            let queues = unsafe { instance.get_physical_device_queue_family_properties(physical) };
            let Some(family) = queues
                .iter()
                .position(|q| q.queue_count > 0 && q.queue_flags.contains(vk::QueueFlags::COMPUTE))
                .map(|n| n as u32)
            else {
                continue;
            };
            let queue = vk::DeviceQueueCreateInfo::default()
                .queue_family_index(family)
                .queue_priorities(&[1.0]);
            let queues = [queue];
            let info = vk::DeviceCreateInfo::default().queue_create_infos(&queues);
            let device = unsafe { instance.create_device(physical, &info, None)? };
            let pool_info = vk::CommandPoolCreateInfo::default()
                .queue_family_index(family)
                .flags(vk::CommandPoolCreateFlags::TRANSIENT);
            let pool = match unsafe { device.create_command_pool(&pool_info, None) } {
                Ok(pool) => pool,
                Err(error) => {
                    unsafe { device.destroy_device(None) };
                    return Err(error.into());
                }
            };
            return Ok((device, physical, family, props, pool));
        }
        Err("No compatible Adreno Vulkan compute device".into())
    }

    pub fn name(&self) -> String {
        // Vulkan guarantees a terminated device name.
        unsafe { CStr::from_ptr(self.props.device_name.as_ptr()) }
            .to_string_lossy()
            .into_owned()
    }

    pub fn buffer(&self, words: usize) -> Result<Buffer<'_>> {
        let bytes = words.checked_mul(4).ok_or("Buffer size overflow")? as u64;
        if bytes == 0 || bytes > self.props.limits.max_storage_buffer_range.into() {
            return Err("Buffer exceeds Vulkan storage limit".into());
        }
        let mut buffer = Buffer {
            gpu: self,
            handle: vk::Buffer::null(),
            memory: vk::DeviceMemory::null(),
            mapped: None,
            words,
        };
        let info = vk::BufferCreateInfo::default()
            .size(bytes)
            .usage(vk::BufferUsageFlags::STORAGE_BUFFER);
        buffer.handle = unsafe { self.device.create_buffer(&info, None)? };
        let req = unsafe { self.device.get_buffer_memory_requirements(buffer.handle) };
        let visible =
            vk::MemoryPropertyFlags::HOST_VISIBLE | vk::MemoryPropertyFlags::HOST_COHERENT;
        let index = (0..self.memory.memory_type_count)
            .find(|&i| {
                req.memory_type_bits & (1 << i) != 0
                    && self.memory.memory_types[i as usize]
                        .property_flags
                        .contains(visible)
            })
            .ok_or("No coherent mapped storage memory")?;
        let allocation = vk::MemoryAllocateInfo::default()
            .allocation_size(req.size)
            .memory_type_index(index);
        buffer.memory = unsafe { self.device.allocate_memory(&allocation, None)? };
        unsafe {
            self.device
                .bind_buffer_memory(buffer.handle, buffer.memory, 0)?;
            let mapped =
                self.device
                    .map_memory(buffer.memory, 0, bytes, vk::MemoryMapFlags::empty())?;
            buffer.mapped = Some(NonNull::new(mapped.cast()).ok_or("Empty Vulkan mapping")?);
            // Give host reads a defined value even before the first dispatch.
            ptr::write_bytes(mapped.cast::<u32>(), 0, words);
        }
        Ok(buffer)
    }

    /// # Safety
    /// The shader must match the declared bindings and push constants. Its
    /// accesses must stay in the bound buffers for every submitted shape.
    pub unsafe fn kernel<'a>(
        &'a self,
        shader: &[u8],
        buffers: &[&'a Buffer<'a>],
        push_words: usize,
    ) -> Result<Kernel<'a>> {
        let bytes = push_words
            .checked_mul(4)
            .ok_or("Push constant size overflow")?;
        if buffers.is_empty()
            || buffers.len() > self.props.limits.max_per_stage_descriptor_storage_buffers as usize
            || bytes == 0
            || bytes > self.props.limits.max_push_constants_size as usize
            || buffers.iter().any(|b| !ptr::eq(b.gpu, self))
        {
            return Err("Invalid Vulkan kernel bindings".into());
        }
        let mut kernel = Kernel {
            gpu: self,
            _buffers: buffers.to_vec(),
            descriptors: vk::DescriptorSetLayout::null(),
            pool: vk::DescriptorPool::null(),
            set: vk::DescriptorSet::null(),
            layout: vk::PipelineLayout::null(),
            pipeline: vk::Pipeline::null(),
            push_words,
        };
        let bindings: Vec<_> = (0..buffers.len() as u32)
            .map(|binding| {
                vk::DescriptorSetLayoutBinding::default()
                    .binding(binding)
                    .descriptor_count(1)
                    .descriptor_type(vk::DescriptorType::STORAGE_BUFFER)
                    .stage_flags(vk::ShaderStageFlags::COMPUTE)
            })
            .collect();
        let layout = vk::DescriptorSetLayoutCreateInfo::default().bindings(&bindings);
        kernel.descriptors = unsafe { self.device.create_descriptor_set_layout(&layout, None)? };
        let sizes = [vk::DescriptorPoolSize {
            ty: vk::DescriptorType::STORAGE_BUFFER,
            descriptor_count: buffers.len() as u32,
        }];
        let pool = vk::DescriptorPoolCreateInfo::default()
            .max_sets(1)
            .pool_sizes(&sizes);
        kernel.pool = unsafe { self.device.create_descriptor_pool(&pool, None)? };
        let sets = [kernel.descriptors];
        let set = vk::DescriptorSetAllocateInfo::default()
            .descriptor_pool(kernel.pool)
            .set_layouts(&sets);
        kernel.set = unsafe { self.device.allocate_descriptor_sets(&set)? }[0];
        for (index, buffer) in buffers.iter().enumerate() {
            let buffer_info = [vk::DescriptorBufferInfo::default()
                .buffer(buffer.handle)
                .range(vk::WHOLE_SIZE)];
            let write = vk::WriteDescriptorSet::default()
                .dst_set(kernel.set)
                .dst_binding(index as u32)
                .descriptor_type(vk::DescriptorType::STORAGE_BUFFER)
                .buffer_info(&buffer_info);
            unsafe { self.device.update_descriptor_sets(&[write], &[]) };
        }
        let constants = [vk::PushConstantRange::default()
            .stage_flags(vk::ShaderStageFlags::COMPUTE)
            .size(bytes as u32)];
        let layout = vk::PipelineLayoutCreateInfo::default()
            .set_layouts(&sets)
            .push_constant_ranges(&constants);
        kernel.layout = unsafe { self.device.create_pipeline_layout(&layout, None)? };
        let code = ash::util::read_spv(&mut Cursor::new(shader))?;
        let module_info = vk::ShaderModuleCreateInfo::default().code(&code);
        let module = unsafe { self.device.create_shader_module(&module_info, None)? };
        let stage = vk::PipelineShaderStageCreateInfo::default()
            .stage(vk::ShaderStageFlags::COMPUTE)
            .module(module)
            .name(c"main");
        let info = vk::ComputePipelineCreateInfo::default()
            .stage(stage)
            .layout(kernel.layout);
        let built = unsafe {
            self.device
                .create_compute_pipelines(vk::PipelineCache::null(), &[info], None)
        };
        unsafe { self.device.destroy_shader_module(module, None) };
        kernel.pipeline = match built {
            Ok(pipelines) => pipelines[0],
            Err((pipelines, error)) => {
                for pipeline in pipelines {
                    unsafe { self.device.destroy_pipeline(pipeline, None) };
                }
                return Err(error.into());
            }
        };
        Ok(kernel)
    }

    /// Record operations in order with a compute memory dependency between them.
    pub fn record<'a>(&'a self, steps: &'a [Dispatch<'a>], repeats: u32) -> Result<Commands<'a>> {
        if steps.is_empty() || !(1..=100).contains(&repeats) {
            return Err("Invalid dispatch count".into());
        }
        for step in steps {
            if !ptr::eq(step.kernel.gpu, self)
                || step.push.len() != step.kernel.push_words
                || step
                    .groups
                    .iter()
                    .zip(self.props.limits.max_compute_work_group_count)
                    .any(|(&n, max)| n == 0 || n > max)
            {
                return Err("Invalid dispatch parameters".into());
            }
        }
        let info = vk::CommandBufferAllocateInfo::default()
            .command_pool(self.pool)
            .command_buffer_count(1);
        let handle = unsafe { self.device.allocate_command_buffers(&info)? }[0];
        let commands = Commands {
            gpu: self,
            handle,
            _bound: PhantomData,
        };
        unsafe {
            self.device
                .begin_command_buffer(handle, &vk::CommandBufferBeginInfo::default())?;
            for repeat in 0..repeats {
                for (index, step) in steps.iter().enumerate() {
                    commands.record(step, repeat + 1 == repeats && index + 1 == steps.len());
                }
            }
            self.device.end_command_buffer(handle)?;
        }
        Ok(commands)
    }

    /// Measure a warm submission. Only use with repeatable, stateless operations.
    pub fn benchmark(&self, steps: &[Dispatch<'_>], repeats: u32) -> Result<Duration> {
        let commands = self.record(steps, repeats)?;
        commands.submit()?;
        let start = Instant::now();
        commands.submit()?;
        Ok(start.elapsed() / repeats)
    }
}

type OpenedDevice = (
    ash::Device,
    vk::PhysicalDevice,
    u32,
    vk::PhysicalDeviceProperties,
    vk::CommandPool,
);

impl Drop for Gpu {
    fn drop(&mut self) {
        unsafe {
            self.device.destroy_command_pool(self.pool, None);
            self.device.destroy_device(None);
            self.instance.destroy_instance(None);
        }
    }
}

pub struct Buffer<'a> {
    gpu: &'a Gpu,
    handle: vk::Buffer,
    memory: vk::DeviceMemory,
    mapped: Option<NonNull<u32>>,
    words: usize,
}

impl Buffer<'_> {
    pub fn words(&self) -> usize {
        self.words
    }

    pub fn write(&self, data: &[u32]) -> Result<()> {
        if data.len() != self.words {
            return Err("Buffer upload length mismatch".into());
        }
        self.write_at(0, data)
    }

    pub fn write_at(&self, offset: usize, data: &[u32]) -> Result<()> {
        if offset > self.words || data.len() > self.words - offset {
            return Err("Buffer upload exceeds storage size".into());
        }
        // Commands only run inside synchronous submit, and the mapping never
        // escapes the single-threaded host API as a Rust reference.
        unsafe {
            let target = self.mapped.unwrap().as_ptr().add(offset);
            ptr::copy_nonoverlapping(data.as_ptr(), target, data.len());
        };
        Ok(())
    }

    pub fn download(&self) -> Vec<u32> {
        // Return a copy so a later dispatch cannot mutate a borrowed Rust slice.
        unsafe { std::slice::from_raw_parts(self.mapped.unwrap().as_ptr(), self.words).to_vec() }
    }
}

impl Drop for Buffer<'_> {
    fn drop(&mut self) {
        unsafe {
            if self.mapped.is_some() {
                self.gpu.device.unmap_memory(self.memory);
            }
            self.gpu.device.destroy_buffer(self.handle, None);
            self.gpu.device.free_memory(self.memory, None);
        }
    }
}

pub struct Kernel<'a> {
    gpu: &'a Gpu,
    _buffers: Vec<&'a Buffer<'a>>,
    descriptors: vk::DescriptorSetLayout,
    pool: vk::DescriptorPool,
    set: vk::DescriptorSet,
    layout: vk::PipelineLayout,
    pipeline: vk::Pipeline,
    push_words: usize,
}

impl Drop for Kernel<'_> {
    fn drop(&mut self) {
        unsafe {
            self.gpu.device.destroy_pipeline(self.pipeline, None);
            self.gpu.device.destroy_pipeline_layout(self.layout, None);
            self.gpu.device.destroy_descriptor_pool(self.pool, None);
            self.gpu
                .device
                .destroy_descriptor_set_layout(self.descriptors, None);
        }
    }
}

pub struct Dispatch<'a> {
    pub kernel: &'a Kernel<'a>,
    pub push: &'a [u32],
    pub groups: [u32; 3],
}

pub struct Commands<'a> {
    gpu: &'a Gpu,
    handle: vk::CommandBuffer,
    _bound: PhantomData<&'a [Dispatch<'a>]>,
}

impl Commands<'_> {
    unsafe fn record(&self, step: &Dispatch<'_>, last: bool) {
        let device = &self.gpu.device;
        let kernel = step.kernel;
        let bind = vk::PipelineBindPoint::COMPUTE;
        let src = vk::PipelineStageFlags::COMPUTE_SHADER;
        let (dst, access) = if last {
            (vk::PipelineStageFlags::HOST, vk::AccessFlags::HOST_READ)
        } else {
            (
                src,
                vk::AccessFlags::SHADER_READ | vk::AccessFlags::SHADER_WRITE,
            )
        };
        let barrier = vk::MemoryBarrier::default()
            .src_access_mask(vk::AccessFlags::SHADER_WRITE)
            .dst_access_mask(access);
        unsafe {
            device.cmd_bind_pipeline(self.handle, bind, kernel.pipeline);
            device.cmd_bind_descriptor_sets(
                self.handle,
                bind,
                kernel.layout,
                0,
                &[kernel.set],
                &[],
            );
            let bytes = std::slice::from_raw_parts(step.push.as_ptr().cast(), step.push.len() * 4);
            device.cmd_push_constants(
                self.handle,
                kernel.layout,
                vk::ShaderStageFlags::COMPUTE,
                0,
                bytes,
            );
            let [x, y, z] = step.groups;
            device.cmd_dispatch(self.handle, x, y, z);
            device.cmd_pipeline_barrier(
                self.handle,
                src,
                dst,
                vk::DependencyFlags::empty(),
                &[barrier],
                &[],
                &[],
            );
        }
    }

    pub fn submit(&self) -> Result<()> {
        let buffers = [self.handle];
        let submit = vk::SubmitInfo::default().command_buffers(&buffers);
        unsafe {
            self.gpu
                .device
                .queue_submit(self.gpu.queue, &[submit], vk::Fence::null())?;
            self.gpu.device.queue_wait_idle(self.gpu.queue)?;
        }
        Ok(())
    }
}

impl Drop for Commands<'_> {
    fn drop(&mut self) {
        unsafe {
            self.gpu
                .device
                .free_command_buffers(self.gpu.pool, &[self.handle])
        };
    }
}
