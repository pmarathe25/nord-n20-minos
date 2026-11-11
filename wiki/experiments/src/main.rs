use std::{env, fs};

use n20_vulkan_probe::{
    Result,
    attention::{self, Decode, Shape},
    projection::{Projection, Shape as MatrixShape},
    vulkan::Gpu,
};

fn compare(actual: &[u32], expected: &[f32]) -> Result<f32> {
    let mut max_error = 0.0_f32;
    for (&bits, &expected) in actual.iter().zip(expected) {
        let error = (f32::from_bits(bits) - expected).abs();
        if !error.is_finite() || error > 0.0002 * (1.0 + expected.abs()) {
            return Err("GPU result differs from scalar reference".into());
        }
        max_error = max_error.max(error);
    }
    Ok(max_error)
}

fn floats(input: &[f32]) -> Vec<u32> {
    input.iter().map(|value| value.to_bits()).collect()
}

fn projection(
    gpu: &Gpu,
    shader: &[u8],
    raw: &[u8],
    rows: u32,
    cols: u32,
    repeats: u32,
) -> Result<()> {
    let shape = MatrixShape {
        rows,
        columns: cols,
    };
    let packed = shape.pack(raw)?;
    let vector: Vec<_> = (0..cols)
        .map(|i| ((i * 13 % 103) as i32 - 51) as f32 / 51.0)
        .collect();
    let expected = shape.reference(raw, &vector)?;
    let weights = gpu.buffer(packed.len())?;
    let input = gpu.buffer(vector.len())?;
    let output = gpu.buffer(rows as usize)?;
    weights.write(&packed)?;
    input.write(&floats(&vector))?;
    // The trusted q2-gemv shader uses these three buffers and this validated shape.
    let operation = unsafe { Projection::bind(gpu, shader, shape, &weights, &input, &output)? };
    let elapsed = gpu.benchmark(&[operation.dispatch()], repeats)?;
    let error = compare(&output.download(), &expected)?;
    println!(
        "{}: Q2_0 {rows}×{cols}, {:.3} ms per product, max absolute error {error:e}",
        gpu.name(),
        elapsed.as_secs_f64() * 1000.0
    );
    Ok(())
}

fn attention(gpu: &Gpu, shader: &[u8], shape: Shape, repeats: u32) -> Result<()> {
    let (q_len, kv_len) = shape.lengths()?;
    let query: Vec<_> = (0..q_len)
        .map(|i| ((i * 53 % 107) as i32 - 53) as f32 / 25.0)
        .collect();
    let keys: Vec<_> = (0..kv_len)
        .map(|i| ((i * 23 % 113) as i32 - 56) as f32 / 32.0)
        .collect();
    let width = shape.head_size as usize;
    let values: Vec<_> = (0..kv_len)
        .map(|i| {
            if i / width % shape.cache_stride as usize >= shape.visible_keys as usize {
                10000.0 // A future cache slot must not affect the result.
            } else {
                ((i * 37 % 127) as i32 - 63) as f32 / 63.0
            }
        })
        .collect();
    let expected = attention::reference(shape, &query, &keys, &values)?;
    let q = gpu.buffer(q_len)?;
    let k = gpu.buffer(kv_len)?;
    let v = gpu.buffer(kv_len)?;
    let output = gpu.buffer(q_len)?;
    q.write(&floats(&query))?;
    k.write(&floats(&keys))?;
    v.write(&floats(&values))?;
    // The trusted attention shader uses the validated causal GQA layout.
    let operation = unsafe { Decode::bind(gpu, shader, shape, &q, &k, &v, &output)? };
    let step = operation.step(shape.visible_keys)?;
    let elapsed = gpu.benchmark(&[step.dispatch()], repeats)?;
    let error = compare(&output.download(), &expected)?;
    // Update a resident cache slot and reuse the same descriptors and pipeline.
    let mut updated = values;
    updated[0] += 0.5;
    v.write_at(0, &[updated[0].to_bits()])?;
    let first = Shape {
        visible_keys: 1,
        ..shape
    };
    let expected_first = attention::reference(first, &query, &keys, &updated)?;
    let first_step = operation.step(first.visible_keys)?;
    gpu.record(&[first_step.dispatch()], 1)?.submit()?;
    compare(&output.download(), &expected_first)?;
    let chained = gpu.buffer(q_len)?;
    let second = unsafe { Decode::bind(gpu, shader, shape, &output, &k, &v, &chained)? };
    let second_step = second.step(shape.visible_keys)?;
    gpu.record(&[first_step.dispatch(), second_step.dispatch()], 1)?
        .submit()?;
    let expected_chained = attention::reference(shape, &expected_first, &keys, &updated)?;
    compare(&chained.download(), &expected_chained)?;
    println!(
        "{}: attention {}/{} heads, width {}, {}/{} visible, {:.3} ms, max absolute error {error:e}",
        gpu.name(),
        shape.heads,
        shape.kv_heads,
        shape.head_size,
        shape.visible_keys,
        shape.cache_stride,
        elapsed.as_secs_f64() * 1000.0
    );
    Ok(())
}

fn main() -> Result<()> {
    let args: Vec<_> = env::args().skip(1).collect();
    match args.as_slice() {
        [mode, shader, tensor, rows, cols, repeats] if mode == "q2" => {
            projection(&Gpu::open_adreno()?, &fs::read(shader)?, &fs::read(tensor)?,
                rows.parse()?, cols.parse()?, repeats.parse()?)
        }
        [mode, shader, heads, kv, width, visible, stride, repeats] if mode == "attention" => {
            let shape = Shape {
                heads: heads.parse()?, kv_heads: kv.parse()?, head_size: width.parse()?,
                visible_keys: visible.parse()?, cache_stride: stride.parse()?,
            };
            attention(&Gpu::open_adreno()?, &fs::read(shader)?, shape, repeats.parse()?)
        }
        _ => Err("Usage: n20-vulkan-probe q2 shader.spv tensor.q2_0 rows columns repeats\n       n20-vulkan-probe attention shader.spv heads kv_heads head_size visible_keys cache_stride repeats".into()),
    }
}
