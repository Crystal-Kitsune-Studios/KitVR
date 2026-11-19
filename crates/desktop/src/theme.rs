use eframe::egui::{self, Color32, Stroke};
use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Deserialize, Serialize)]
pub enum ThemeChoice {
    #[default]
    Classic,
    Midnight,
    Blossom,
    Custom,
}

#[derive(Clone, Debug, PartialEq, Eq, Deserialize, Serialize)]
#[serde(default)]
pub struct ThemeSettings {
    pub choice: ThemeChoice,
    pub background: [u8; 3],
    pub surface: [u8; 3],
    pub text: [u8; 3],
    pub accent: [u8; 3],
}

impl Default for ThemeSettings {
    fn default() -> Self {
        Self {
            choice: ThemeChoice::Classic,
            background: [0x24, 0x26, 0x30],
            surface: [0x55, 0x58, 0x8E],
            text: [0xE9, 0xE5, 0xD8],
            accent: [0xE9, 0xC8, 0x3C],
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct Palette {
    pub ink: Color32,
    pub header: Color32,
    pub surface: Color32,
    pub raised: Color32,
    pub sunken: Color32,
    pub line: Color32,
    pub text: Color32,
    pub muted: Color32,
    pub dim: Color32,
    pub accent: Color32,
    pub accent_ink: Color32,
    pub accent_text: Color32,
    pub faint: Color32,
    pub hovered: Color32,
    pub light_surface: bool,
    pub badge: Color32,
    pub badge_line: Color32,
    pub step_text: Color32,
}

fn color(rgb: [u8; 3]) -> Color32 {
    Color32::from_rgb(rgb[0], rgb[1], rgb[2])
}

fn shift(rgb: [u8; 3], offset: [i16; 3]) -> Color32 {
    color(std::array::from_fn(|i| {
        (rgb[i] as i16 + offset[i]).clamp(0, 255) as u8
    }))
}

fn blend(a: [u8; 3], b: [u8; 3], ratio: f32) -> Color32 {
    color(std::array::from_fn(|i| {
        (a[i] as f32 * (1.0 - ratio) + b[i] as f32 * ratio).round() as u8
    }))
}

fn luminance(rgb: [u8; 3]) -> f32 {
    let linear = rgb.map(|component| {
        let value = component as f32 / 255.0;
        if value <= 0.04045 {
            value / 12.92
        } else {
            ((value + 0.055) / 1.055).powf(2.4)
        }
    });
    0.2126 * linear[0] + 0.7152 * linear[1] + 0.0722 * linear[2]
}

impl ThemeSettings {
    pub fn palette(&self) -> Palette {
        let (background, surface, text, accent) = match self.choice {
            ThemeChoice::Classic => ([14, 20, 22], [22, 30, 33], [232, 237, 236], [242, 166, 90]),
            ThemeChoice::Midnight => ([12, 16, 31], [22, 28, 48], [232, 236, 250], [126, 158, 255]),
            ThemeChoice::Blossom => ([28, 17, 26], [43, 26, 39], [250, 234, 241], [244, 145, 185]),
            ThemeChoice::Custom => (self.background, self.surface, self.text, self.accent),
        };
        let classic = self.choice == ThemeChoice::Classic;
        let light_surface = luminance(surface) > 0.179;
        let direction = if light_surface { -1 } else { 1 };
        Palette {
            ink: color(background),
            header: shift(surface, [2 * direction, 3 * direction, 3 * direction]),
            surface: color(surface),
            raised: shift(surface, [6 * direction, 8 * direction, 8 * direction]),
            sunken: shift(background, [-3 * direction; 3]),
            line: shift(surface, [20 * direction, 24 * direction, 25 * direction]),
            text: color(text),
            muted: if classic {
                color([147, 162, 163])
            } else {
                blend(text, surface, 0.4)
            },
            dim: if classic {
                color([130, 145, 143])
            } else {
                blend(text, surface, 0.5)
            },
            accent: color(accent),
            accent_ink: if classic {
                color([26, 18, 6])
            } else if luminance(accent) > 0.179 {
                Color32::BLACK
            } else {
                Color32::WHITE
            },
            accent_text: if classic {
                color([245, 192, 138])
            } else {
                blend(accent, text, 0.3)
            },
            faint: shift(background, [4 * direction, 5 * direction, 5 * direction]),
            hovered: shift(surface, [16 * direction, 20 * direction, 20 * direction]),
            light_surface,
            badge: if classic {
                color([46, 37, 26])
            } else {
                blend(accent, surface, 0.85)
            },
            badge_line: if classic {
                color([122, 88, 52])
            } else {
                blend(accent, surface, 0.5)
            },
            step_text: if classic {
                color([203, 214, 212])
            } else {
                color(text)
            },
        }
    }
}

impl Palette {
    pub fn visuals(self) -> egui::Visuals {
        let mut visuals = if self.light_surface {
            egui::Visuals::light()
        } else {
            egui::Visuals::dark()
        };
        visuals.panel_fill = self.ink;
        visuals.window_fill = self.surface;
        visuals.extreme_bg_color = self.sunken;
        visuals.faint_bg_color = self.faint;
        visuals.window_stroke = Stroke::new(1.0_f32, self.line);
        visuals.widgets.noninteractive.bg_fill = self.surface;
        visuals.widgets.noninteractive.fg_stroke = Stroke::new(1.0_f32, self.text);
        visuals.widgets.inactive.bg_fill = self.raised;
        visuals.widgets.inactive.fg_stroke = Stroke::new(1.0_f32, self.text);
        visuals.widgets.inactive.rounding = egui::Rounding::same(8.0);
        visuals.widgets.hovered.bg_fill = self.hovered;
        visuals.widgets.hovered.fg_stroke = Stroke::new(1.0_f32, self.text);
        visuals.widgets.hovered.rounding = egui::Rounding::same(8.0);
        visuals.widgets.active.bg_fill = self.accent;
        visuals.widgets.active.fg_stroke = Stroke::new(1.0_f32, self.accent_ink);
        visuals.widgets.active.rounding = egui::Rounding::same(8.0);
        visuals.selection.bg_fill =
            Color32::from_rgba_unmultiplied(self.accent.r(), self.accent.g(), self.accent.b(), 60);
        visuals.selection.stroke = Stroke::new(1.0_f32, self.accent_text);
        visuals.window_rounding = egui::Rounding::same(12.0);
        visuals.menu_rounding = egui::Rounding::same(8.0);
        visuals
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn old_settings_and_custom_colors_round_trip() {
        let old: ThemeSettings = serde_json::from_str("{}").unwrap();
        assert_eq!(old, ThemeSettings::default());
        let custom = ThemeSettings {
            choice: ThemeChoice::Custom,
            accent: [8, 20, 100],
            ..old
        };
        let json = serde_json::to_string(&custom).unwrap();
        assert_eq!(
            serde_json::from_str::<ThemeSettings>(&json).unwrap(),
            custom
        );
        assert_eq!(custom.palette().accent, Color32::from_rgb(8, 20, 100));
        assert_eq!(custom.palette().accent_ink, Color32::WHITE);
    }

    #[test]
    fn presets_do_not_overwrite_custom_colors() {
        let mut settings = ThemeSettings {
            accent: [1, 2, 3],
            ..Default::default()
        };
        settings.choice = ThemeChoice::Midnight;
        assert_ne!(settings.palette().accent, Color32::from_rgb(1, 2, 3));
        settings.choice = ThemeChoice::Custom;
        assert_eq!(settings.palette().accent, Color32::from_rgb(1, 2, 3));
    }

    #[test]
    fn first_custom_palette_uses_the_requested_colors() {
        let mut settings = ThemeSettings::default();
        settings.choice = ThemeChoice::Custom;
        let palette = settings.palette();
        assert_eq!(palette.ink, Color32::from_rgb(0x24, 0x26, 0x30));
        assert_eq!(palette.surface, Color32::from_rgb(0x55, 0x58, 0x8E));
        assert_eq!(palette.text, Color32::from_rgb(0xE9, 0xE5, 0xD8));
        assert_eq!(palette.accent, Color32::from_rgb(0xE9, 0xC8, 0x3C));
    }
}
