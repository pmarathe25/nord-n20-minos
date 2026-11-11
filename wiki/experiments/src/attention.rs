//! Causal grouped-query attention for one query token.

use crate::{
    Result,
    vulkan::{Buffer, Dispatch, Gpu, Kernel},
};

#[derive(Clone, Copy)]
pub struct Shape {
    pub heads: u32,
    pub kv_heads: u32,
    pub head_size: u32,
    pub visible_keys: u32,
    pub cache_stride: u32,
}

impl Shape {
    pub fn lengths(self) -> Result<(usize, usize)> {
        if !(1..=128).contains(&self.heads)
            || self.kv_heads == 0
            || !self.heads.is_multiple_of(self.kv_heads)
            || !(16..=256).contains(&self.head_size)
            || !self.head_size.is_multiple_of(16)
            || !(1..=2048).contains(&self.visible_keys)
            || !(self.visible_keys..=2048).contains(&self.cache_stride)
        {
            return Err("Unsupported attention shape".into());
        }
        let q = self.heads * self.head_size;
        let kv = self.kv_heads * self.cache_stride * self.head_size;
        Ok((q as usize, kv as usize))
    }
}

pub struct Decode<'a> {
    kernel: Kernel<'a>,
    shape: [u32; 5],
}

impl<'a> Decode<'a> {
    /// # Safety
    /// Use the compiled `attention.comp` shader with this buffer layout.
    pub unsafe fn bind(
        gpu: &'a Gpu,
        shader: &[u8],
        shape: Shape,
        query: &'a Buffer<'a>,
        keys: &'a Buffer<'a>,
        values: &'a Buffer<'a>,
        output: &'a Buffer<'a>,
    ) -> Result<Self> {
        let (q_len, kv_len) = shape.lengths()?;
        if query.words() != q_len
            || output.words() != q_len
            || keys.words() != kv_len
            || values.words() != kv_len
        {
            return Err("Attention buffer length mismatch".into());
        }
        let shape = [
            shape.heads,
            shape.kv_heads,
            shape.head_size,
            shape.visible_keys,
            shape.cache_stride,
        ];
        let kernel = unsafe { gpu.kernel(shader, &[query, keys, values, output], shape.len())? };
        Ok(Self { kernel, shape })
    }

    pub fn step(&self, visible_keys: u32) -> Result<DecodeStep<'_>> {
        if !(1..=self.shape[4]).contains(&visible_keys) {
            return Err("Visible attention prefix exceeds cache".into());
        }
        let mut push = self.shape;
        push[3] = visible_keys;
        Ok(DecodeStep {
            kernel: &self.kernel,
            push,
        })
    }
}

pub struct DecodeStep<'a> {
    kernel: &'a Kernel<'a>,
    push: [u32; 5],
}

impl DecodeStep<'_> {
    pub fn dispatch(&self) -> Dispatch<'_> {
        Dispatch {
            kernel: self.kernel,
            push: &self.push,
            groups: [self.push[0], 1, 1],
        }
    }
}

/// Scalar reference for validation. Q and output use [head, channel].
/// K and V use [kv head, token slot, channel].
pub fn reference(shape: Shape, query: &[f32], keys: &[f32], values: &[f32]) -> Result<Vec<f32>> {
    let (q_len, kv_len) = shape.lengths()?;
    if query.len() != q_len || keys.len() != kv_len || values.len() != kv_len {
        return Err("Attention reference length mismatch".into());
    }
    let width = shape.head_size as usize;
    let visible = shape.visible_keys as usize;
    let stride = shape.cache_stride as usize * width;
    let heads_per_kv = (shape.heads / shape.kv_heads) as usize;
    let scale = 1.0 / (width as f64).sqrt();
    let mut output = vec![0.0; q_len];
    let mut scores = vec![0.0; visible];
    for (head, q) in query.chunks_exact(width).enumerate() {
        let start = head / heads_per_kv * stride;
        let end = start + visible * width;
        for (score, key) in scores.iter_mut().zip(keys[start..end].chunks_exact(width)) {
            *score = q
                .iter()
                .zip(key)
                .map(|(&q, &k)| f64::from(q) * f64::from(k))
                .sum::<f64>()
                * scale;
        }
        let max = scores.iter().copied().fold(f64::NEG_INFINITY, f64::max);
        for score in &mut scores {
            *score = (*score - max).exp();
        }
        let denominator = scores.iter().sum::<f64>();
        for (channel, result) in output[head * width..(head + 1) * width]
            .iter_mut()
            .enumerate()
        {
            *result = (scores
                .iter()
                .zip(values[start + channel..end].iter().step_by(width))
                .map(|(&weight, &value)| weight * f64::from(value))
                .sum::<f64>()
                / denominator) as f32;
        }
    }
    Ok(output)
}
