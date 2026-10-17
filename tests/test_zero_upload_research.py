import importlib.util
import json
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("zero_upload", ROOT / "research/zero_upload_limit.py")
research = importlib.util.module_from_spec(spec)
spec.loader.exec_module(research)

SOURCE = '''CreationArgs GetImmCtxArgs(Adapter *adapter, UINT flags)
{
    CreationArgs args = {};
    args.MaxAllocatedUploadHeapSpacePerCommandList = MAXDWORD;
    args.UseNonBlockingPSOs = GetCompatValue("NonBlockingPSOs", &value) && value;
    return args;
}
'''


class ZeroUploadResearchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.source = self.root / "production"
        (self.source / "src").mkdir(parents=True)
        (self.source / "src/device.cpp").write_text(SOURCE)
        (self.source / "other.txt").write_bytes(b"unchanged\r\n")

    def test_variant_changes_only_the_limit_and_remains_rejected_by_gate(self):
        output = self.root / "research"
        research.make_variant(self.source, output)
        self.assertEqual((self.source / "src/device.cpp").read_text(), SOURCE)
        variant = (output / "src/device.cpp").read_text()
        self.assertEqual(variant, SOURCE.replace("= MAXDWORD;", "= 0u;"))
        self.assertEqual((output / "other.txt").read_bytes(), b"unchanged\r\n")
        self.assertEqual(research.port.check_immediate_context_args("production", SOURCE), [])
        errors = research.port.check_immediate_context_args("research", variant)
        self.assertEqual(len(errors), 1)
        self.assertIn("non-zero", errors[0])
        manifest = json.loads((output / "RESEARCH-ONLY.json").read_text())
        self.assertTrue(manifest["researchOnly"])
        self.assertNotEqual(manifest["productionDeviceSHA256"], manifest["variantDeviceSHA256"])

    def test_existing_output_is_preserved(self):
        with self.assertRaises(FileExistsError):
            research.make_variant(self.source, self.root)
        self.assertEqual((self.source / "src/device.cpp").read_text(), SOURCE)

    def test_nested_output_is_refused(self):
        with self.assertRaises(ValueError):
            research.make_variant(self.source, self.source / "research")
        self.assertFalse((self.source / "research").exists())

    def test_invalid_production_input_does_not_create_output(self):
        (self.source / "src/device.cpp").write_text(SOURCE.replace("MAXDWORD", "0u"))
        output = self.root / "research"
        with self.assertRaises(ValueError):
            research.make_variant(self.source, output)
        self.assertFalse(output.exists())
