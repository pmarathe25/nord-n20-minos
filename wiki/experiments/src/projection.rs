//! Packed GGML Q2_0 projection with FP32 activations and accumulation.

use half::f16;

use crate::{
    Result,
    vulkan::{Buffer, Dispatch, Gpu, Kernel},
};

const BLOCK_WIDTH: u32 = 64;
const BLOCK_BYTES: usize = 18;

#[derive(Clone, Copy)]
pub struct Shape {
    pub rows: u32,
    pub columns: u32,
}

impl Shape {
    fn blocks(self) -> Result<usize> {
        if !(1..=65535).contains(&self.rows)
            || !(BLOCK_WIDTH..=65536).contains(&self.columns)
            || !self.columns.is_multiple_of(BLOCK_WIDTH)
            || u64::from(self.rows) * u64::from(self.columns) > 256 * 1024 * 1024
        {
            return Err("Unsupported Q2_0 tensor shape".into());
        }
        Ok((self.rows * (self.columns / BLOCK_WIDTH)) as usize)
    }

    pub fn pack(self, raw: &[u8]) -> Result<Vec<u32>> {
        let blocks = self.blocks()?;
        if raw.len() != blocks * BLOCK_BYTES {
            return Err("Q2_0 tensor size mismatch".into());
        }
        let scale_start = 4 * blocks;
        let mut packed = vec![0; scale_start + blocks.div_ceil(2)];
        for (index, block) in raw.as_chunks::<BLOCK_BYTES>().0.iter().enumerate() {
            let scale = u16::from_le_bytes([block[0], block[1]]);
            if !f16::from_bits(scale).is_finite() {
                return Err("Nonfinite Q2_0 scale".into());
            }
            packed[scale_start + index / 2] |= u32::from(scale) << (16 * (index % 2));
            for (word, &bytes) in block[2..].as_chunks::<4>().0.iter().enumerate() {
                packed[4 * index + word] = u32::from_le_bytes(bytes);
            }
        }
        Ok(packed)
    }

    /// Independent scalar reference over the original interleaved GGUF blocks.
    pub fn reference(self, raw: &[u8], input: &[f32]) -> Result<Vec<f32>> {
        if raw.len() != self.blocks()? * BLOCK_BYTES || input.len() != self.columns as usize {
            return Err("Q2_0 reference length mismatch".into());
        }
        let row_bytes = (self.columns / BLOCK_WIDTH) as usize * BLOCK_BYTES;
        Ok(raw
            .chunks_exact(row_bytes)
            .map(|row| {
                let mut sum = 0.0;
                for (block, activation) in row
                    .as_chunks::<BLOCK_BYTES>()
                    .0
                    .iter()
                    .zip(input.as_chunks::<{ BLOCK_WIDTH as usize }>().0)
                {
                    let scale = f16::from_le_bytes([block[0], block[1]]).to_f64();
                    for (i, &input) in activation.iter().enumerate() {
                        let code = (block[2 + i / 4] >> (2 * (i % 4))) & 3;
                        sum += f64::from(i32::from(code) - 1) * scale * f64::from(input);
                    }
                }
                sum as f32
            })
            .collect())
    }
}

pub struct Projection<'a> {
    kernel: Kernel<'a>,
    push: [u32; 3],
}

impl<'a> Projection<'a> {
    /// # Safety
    /// Use the compiled `q2-gemv.comp` shader with this packed layout.
    pub unsafe fn bind(
        gpu: &'a Gpu,
        shader: &[u8],
        shape: Shape,
        weights: &'a Buffer<'a>,
        input: &'a Buffer<'a>,
        output: &'a Buffer<'a>,
    ) -> Result<Self> {
        let blocks = shape.blocks()?;
        let scale_start = 4 * blocks;
        if weights.words() != scale_start + blocks.div_ceil(2)
            || input.words() != shape.columns as usize
            || output.words() != shape.rows as usize
        {
            return Err("Q2_0 buffer length mismatch".into());
        }
        let push = [shape.columns / BLOCK_WIDTH, scale_start as u32, shape.rows];
        let kernel = unsafe { gpu.kernel(shader, &[weights, input, output], push.len())? };
        Ok(Self { kernel, push })
    }

    pub fn dispatch(&self) -> Dispatch<'_> {
        Dispatch {
            kernel: &self.kernel,
            push: &self.push,
            groups: [self.push[2].div_ceil(8), 1, 1],
        }
    }
}
