import json
import pathlib
import unittest


INPUT = pathlib.Path(__file__).resolve().parents[1] / "driver" / "resources" / "input"


class InputBindingsTest(unittest.TestCase):
    def test_bindings_reference_exposed_controller_components(self):
        profile = json.loads((INPUT / "kitvr_controller_profile.json").read_text())
        self.assertEqual(profile["controller_type"], "kitvr_touch")
        legacy = profile["legacy_binding"].replace("{kitvr}/input/", "")
        system = next(
            item["binding_url"] for item in profile["default_bindings"]
            if item["app_key"] == "openvr.component.vrcompositor"
        )
        for filename in (legacy, system):
            bindings = json.loads((INPUT / filename).read_text())
            self.assertEqual(bindings["controller_type"], profile["controller_type"])
            for action_set in bindings["bindings"].values():
                for entry in action_set.get("sources", []) + action_set.get("poses", []):
                    path = entry["path"]
                    self.assertTrue(path.startswith(("/user/hand/left/", "/user/hand/right/")))
                    component = path[path.index("/", len("/user/hand/")):]
                    self.assertIn(component, profile["input_source"])

    def test_legacy_bindings_cover_both_hands(self):
        bindings = json.loads((INPUT / "legacy_bindings_kitvr_touch.json").read_text())
        legacy = bindings["bindings"]["/actions/legacy"]
        paths = {source["path"] for source in legacy["sources"]}
        poses = {pose["path"] for pose in legacy["poses"]}
        for hand in ("left", "right"):
            self.assertIn(f"/user/hand/{hand}/pose/raw", poses)
            for control in ("trigger", "grip", "joystick", "primary", "secondary"):
                self.assertIn(f"/user/hand/{hand}/input/{control}", paths)

    def test_dashboard_mouse_has_distinct_pinch_and_grip_actions(self):
        bindings = json.loads((INPUT / "vrcompositor_bindings_kitvr_touch.json").read_text())
        sources = bindings["bindings"]["/actions/lasermouse"]["sources"]
        clicks = {
            source["path"]: source["inputs"]["click"]["output"]
            for source in sources
        }
        for hand in ("left", "right"):
            self.assertEqual(clicks[f"/user/hand/{hand}/input/trigger"],
                             "/actions/lasermouse/in/leftclick")
            self.assertEqual(clicks[f"/user/hand/{hand}/input/grip"],
                             "/actions/lasermouse/in/rightclick")


if __name__ == "__main__":
    unittest.main()
