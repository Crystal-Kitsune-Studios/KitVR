//! Draws the skinned avatar into eframe's framebuffer from an egui paint callback.
//!
//! Vertices are skinned on the CPU and streamed every frame, which keeps the shader simple and
//! works on any GL 2.1+/ES 2.0 driver eframe can start. Shading is a two-tone toon look that
//! approximates MToon. Output is premultiplied alpha so the window stays see-through around the
//! avatar.

use std::sync::Arc;

use eframe::egui_glow::ShaderVersion;
use eframe::glow::{self, HasContext};
use glam::Mat4;

use crate::pose;
use crate::vrm::{AlphaMode, Model};

struct GpuPrimitive {
    vbo: glow::Buffer,
    ebo: glow::Buffer,
    vao: Option<glow::VertexArray>,
    count: i32,
}

pub struct Renderer {
    program: glow::Program,
    primitives: Vec<GpuPrimitive>,
    textures: Vec<glow::Texture>,
    white: glow::Texture,
    scratch: Vec<f32>,
    u_mvp: Option<glow::UniformLocation>,
    u_color: Option<glow::UniformLocation>,
    u_cutoff: Option<glow::UniformLocation>,
    u_mode: Option<glow::UniformLocation>,
    u_tex: Option<glow::UniformLocation>,
    a_pos: u32,
    a_normal: u32,
    a_uv: u32,
}

fn shader_sources(version: ShaderVersion) -> (String, String) {
    let header = version.version_declaration();
    let precision = if version.is_embedded() { "precision mediump float;\n" } else { "" };
    let (attribute, varying_out, varying_in, texture, frag_out, frag_color) = if version.is_new_shader_interface() {
        ("in", "out", "in", "texture", "out vec4 f_color;\n", "f_color")
    } else {
        ("attribute", "varying", "varying", "texture2D", "", "gl_FragColor")
    };
    let vertex = format!(
        "{header}{precision}\
         {attribute} vec3 a_pos;\n{attribute} vec3 a_normal;\n{attribute} vec2 a_uv;\n\
         uniform mat4 u_mvp;\n\
         {varying_out} vec3 v_normal;\n{varying_out} vec2 v_uv;\n\
         void main() {{ v_normal = a_normal; v_uv = a_uv; gl_Position = u_mvp * vec4(a_pos, 1.0); }}\n"
    );
    let fragment = format!(
        "{header}{precision}\
         uniform sampler2D u_tex;\nuniform vec4 u_color;\nuniform float u_cutoff;\nuniform float u_mode;\n\
         {varying_in} vec3 v_normal;\n{varying_in} vec2 v_uv;\n{frag_out}\
         void main() {{\n\
           vec4 c = {texture}(u_tex, v_uv) * u_color;\n\
           if (u_mode > 0.5 && u_mode < 1.5 && c.a < u_cutoff) discard;\n\
           if (u_mode < 1.5) c.a = 1.0;\n\
           if (c.a < 0.004) discard;\n\
           vec3 n = normalize(v_normal);\n\
           if (!gl_FrontFacing) n = -n;\n\
           float d = dot(n, normalize(vec3(0.35, 0.55, 0.75)));\n\
           float shade = mix(0.80, 1.0, smoothstep(-0.05, 0.12, d));\n\
           {frag_color} = vec4(c.rgb * shade * c.a, c.a);\n\
         }}\n"
    );
    (vertex, fragment)
}

unsafe fn compile(gl: &glow::Context, kind: u32, source: &str) -> Result<glow::Shader, String> {
    let shader = gl.create_shader(kind)?;
    gl.shader_source(shader, source);
    gl.compile_shader(shader);
    if !gl.get_shader_compile_status(shader) {
        let log = gl.get_shader_info_log(shader);
        gl.delete_shader(shader);
        return Err(format!("shader compile failed: {log}"));
    }
    Ok(shader)
}

impl Renderer {
    pub fn new(gl: &glow::Context, model: &Model) -> Result<Self, String> {
        unsafe {
            let (vertex, fragment) = shader_sources(ShaderVersion::get(gl));
            let program = gl.create_program()?;
            let vs = compile(gl, glow::VERTEX_SHADER, &vertex)?;
            let fs = compile(gl, glow::FRAGMENT_SHADER, &fragment)?;
            gl.attach_shader(program, vs);
            gl.attach_shader(program, fs);
            gl.bind_attrib_location(program, 0, "a_pos");
            gl.bind_attrib_location(program, 1, "a_normal");
            gl.bind_attrib_location(program, 2, "a_uv");
            gl.link_program(program);
            gl.detach_shader(program, vs);
            gl.detach_shader(program, fs);
            gl.delete_shader(vs);
            gl.delete_shader(fs);
            if !gl.get_program_link_status(program) {
                let log = gl.get_program_info_log(program);
                gl.delete_program(program);
                return Err(format!("shader link failed: {log}"));
            }

            let upload = |width: u32, height: u32, rgba: &[u8]| -> Result<glow::Texture, String> {
                let texture = gl.create_texture()?;
                gl.bind_texture(glow::TEXTURE_2D, Some(texture));
                gl.pixel_store_i32(glow::UNPACK_ALIGNMENT, 1);
                gl.tex_image_2d(
                    glow::TEXTURE_2D, 0, glow::RGBA as i32, width as i32, height as i32, 0,
                    glow::RGBA, glow::UNSIGNED_BYTE, Some(rgba),
                );
                gl.generate_mipmap(glow::TEXTURE_2D);
                gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_MIN_FILTER, glow::LINEAR_MIPMAP_LINEAR as i32);
                gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_MAG_FILTER, glow::LINEAR as i32);
                gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_WRAP_S, glow::REPEAT as i32);
                gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_WRAP_T, glow::REPEAT as i32);
                Ok(texture)
            };
            let white = upload(1, 1, &[255, 255, 255, 255])?;
            let textures = model
                .textures
                .iter()
                .map(|t| upload(t.width, t.height, &t.rgba))
                .collect::<Result<Vec<_>, _>>()?;

            let mut primitives = Vec::new();
            for primitive in &model.primitives {
                let vbo = gl.create_buffer()?;
                let ebo = gl.create_buffer()?;
                let vao = gl.create_vertex_array().ok();
                if let Some(vao) = vao {
                    gl.bind_vertex_array(Some(vao));
                }
                gl.bind_buffer(glow::ARRAY_BUFFER, Some(vbo));
                gl.buffer_data_size(
                    glow::ARRAY_BUFFER, (primitive.positions.len() * 8 * 4) as i32, glow::STREAM_DRAW,
                );
                gl.bind_buffer(glow::ELEMENT_ARRAY_BUFFER, Some(ebo));
                gl.buffer_data_u8_slice(
                    glow::ELEMENT_ARRAY_BUFFER, bytes_of_u32(&primitive.indices), glow::STATIC_DRAW,
                );
                if vao.is_some() {
                    Self::bind_attributes(gl);
                    gl.bind_vertex_array(None);
                }
                gl.bind_buffer(glow::ARRAY_BUFFER, None);
                gl.bind_buffer(glow::ELEMENT_ARRAY_BUFFER, None);
                primitives.push(GpuPrimitive { vbo, ebo, vao, count: primitive.indices.len() as i32 });
            }

            Ok(Self {
                u_mvp: gl.get_uniform_location(program, "u_mvp"),
                u_color: gl.get_uniform_location(program, "u_color"),
                u_cutoff: gl.get_uniform_location(program, "u_cutoff"),
                u_mode: gl.get_uniform_location(program, "u_mode"),
                u_tex: gl.get_uniform_location(program, "u_tex"),
                program,
                primitives,
                textures,
                white,
                scratch: Vec::new(),
                a_pos: 0,
                a_normal: 1,
                a_uv: 2,
            })
        }
    }

    unsafe fn bind_attributes(gl: &glow::Context) {
        let stride = 8 * 4;
        gl.enable_vertex_attrib_array(0);
        gl.vertex_attrib_pointer_f32(0, 3, glow::FLOAT, false, stride, 0);
        gl.enable_vertex_attrib_array(1);
        gl.vertex_attrib_pointer_f32(1, 3, glow::FLOAT, false, stride, 3 * 4);
        gl.enable_vertex_attrib_array(2);
        gl.vertex_attrib_pointer_f32(2, 2, glow::FLOAT, false, stride, 6 * 4);
    }

    /// Draws the avatar posed by `world` with `mvp`. If `probe` is a framebuffer pixel (origin at
    /// the bottom left), returns whether the avatar covers it.
    pub fn paint(
        &mut self,
        gl: &glow::Context,
        model: &Model,
        world: &[Mat4],
        expressions: &std::collections::HashMap<crate::vrm::Expression, f32>,
        mvp: Mat4,
        probe: Option<[i32; 2]>,
    ) -> Option<bool> {
        unsafe {
            gl.enable(glow::DEPTH_TEST);
            gl.depth_func(glow::LEQUAL);
            gl.depth_mask(true);
            gl.clear_depth_f32(1.0);
            gl.clear(glow::DEPTH_BUFFER_BIT);
            gl.enable(glow::BLEND);
            gl.blend_equation(glow::FUNC_ADD);
            gl.blend_func(glow::ONE, glow::ONE_MINUS_SRC_ALPHA);
            gl.use_program(Some(self.program));
            gl.uniform_matrix_4_f32_slice(self.u_mvp.as_ref(), false, &mvp.to_cols_array());
            gl.uniform_1_i32(self.u_tex.as_ref(), 0);
            gl.active_texture(glow::TEXTURE0);

            // Opaque and cut-out first, blended last.
            for pass in [false, true] {
                for (index, primitive) in model.primitives.iter().enumerate() {
                    let material = primitive.material.and_then(|m| model.materials.get(m)).cloned().unwrap_or_default();
                    if (material.alpha_mode == AlphaMode::Blend) != pass {
                        continue;
                    }
                    let morphs = pose::morph_weights(model, primitive, expressions);
                    pose::skin_primitive(model, world, primitive, &morphs, &mut self.scratch);
                    let gpu = &self.primitives[index];
                    gl.bind_buffer(glow::ARRAY_BUFFER, Some(gpu.vbo));
                    gl.buffer_sub_data_u8_slice(glow::ARRAY_BUFFER, 0, bytes_of_f32(&self.scratch));
                    match gpu.vao {
                        Some(vao) => gl.bind_vertex_array(Some(vao)),
                        None => {
                            gl.bind_buffer(glow::ELEMENT_ARRAY_BUFFER, Some(gpu.ebo));
                            Self::bind_attributes(gl);
                        }
                    }
                    if material.double_sided {
                        gl.disable(glow::CULL_FACE);
                    } else {
                        gl.enable(glow::CULL_FACE);
                        gl.cull_face(glow::BACK);
                        gl.front_face(glow::CCW);
                    }
                    let texture = material.texture.and_then(|t| self.textures.get(t)).copied().unwrap_or(self.white);
                    gl.bind_texture(glow::TEXTURE_2D, Some(texture));
                    gl.uniform_4_f32_slice(self.u_color.as_ref(), &material.base_color);
                    gl.uniform_1_f32(self.u_cutoff.as_ref(), material.cutoff);
                    let mode = match material.alpha_mode {
                        AlphaMode::Opaque => 0.0,
                        AlphaMode::Mask => 1.0,
                        AlphaMode::Blend => 2.0,
                    };
                    gl.uniform_1_f32(self.u_mode.as_ref(), mode);
                    gl.draw_elements(glow::TRIANGLES, gpu.count, glow::UNSIGNED_INT, 0);
                    if gpu.vao.is_some() {
                        gl.bind_vertex_array(None);
                    } else {
                        for attribute in [self.a_pos, self.a_normal, self.a_uv] {
                            gl.disable_vertex_attrib_array(attribute);
                        }
                    }
                }
            }

            let hit = probe.map(|[x, y]| {
                let mut pixel = [0u8; 4];
                gl.read_pixels(x, y, 1, 1, glow::RGBA, glow::UNSIGNED_BYTE, glow::PixelPackData::Slice(&mut pixel));
                pixel[3] > 24
            });

            gl.disable(glow::DEPTH_TEST);
            gl.disable(glow::CULL_FACE);
            gl.bind_buffer(glow::ARRAY_BUFFER, None);
            gl.bind_buffer(glow::ELEMENT_ARRAY_BUFFER, None);
            gl.bind_texture(glow::TEXTURE_2D, None);
            gl.use_program(None);
            hit
        }
    }

    pub fn destroy(&self, gl: &glow::Context) {
        unsafe {
            gl.delete_program(self.program);
            for primitive in &self.primitives {
                gl.delete_buffer(primitive.vbo);
                gl.delete_buffer(primitive.ebo);
                if let Some(vao) = primitive.vao {
                    gl.delete_vertex_array(vao);
                }
            }
            for texture in &self.textures {
                gl.delete_texture(*texture);
            }
            gl.delete_texture(self.white);
        }
    }
}

fn bytes_of_f32(values: &[f32]) -> &[u8] {
    unsafe { std::slice::from_raw_parts(values.as_ptr() as *const u8, std::mem::size_of_val(values)) }
}

fn bytes_of_u32(values: &[u32]) -> &[u8] {
    unsafe { std::slice::from_raw_parts(values.as_ptr() as *const u8, std::mem::size_of_val(values)) }
}

/// Shared between the egui update and the paint callback.
pub struct Scene {
    pub model: Option<Arc<Model>>,
    pub generation: u64,
    pub world: Vec<Mat4>,
    pub expressions: std::collections::HashMap<crate::vrm::Expression, f32>,
    pub mvp: Mat4,
    pub probe: Option<[i32; 2]>,
    pub hit: bool,
    pub renderer: Option<(u64, Renderer)>,
    pub error: Option<String>,
}

impl Default for Scene {
    fn default() -> Self {
        Self {
            model: None,
            generation: 0,
            world: Vec::new(),
            expressions: Default::default(),
            mvp: Mat4::IDENTITY,
            probe: None,
            hit: false,
            renderer: None,
            error: None,
        }
    }
}

impl Scene {
    pub fn paint(&mut self, gl: &glow::Context) {
        let Some(model) = self.model.clone() else { return };
        if self.world.len() != model.nodes.len() {
            return;
        }
        if self.renderer.as_ref().map(|(generation, _)| *generation) != Some(self.generation) {
            if let Some((_, old)) = self.renderer.take() {
                old.destroy(gl);
            }
            match Renderer::new(gl, &model) {
                Ok(renderer) => self.renderer = Some((self.generation, renderer)),
                Err(error) => {
                    self.error = Some(error);
                    self.model = None;
                    return;
                }
            }
        }
        let probe = self.probe;
        if let Some((_, renderer)) = self.renderer.as_mut() {
            if let Some(hit) = renderer.paint(gl, &model, &self.world, &self.expressions, self.mvp, probe) {
                self.hit = hit;
            } else {
                self.hit = false;
            }
        }
    }
}
