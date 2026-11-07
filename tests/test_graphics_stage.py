import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('graphics_stage', Path(__file__).resolve().parents[1] / 'scripts/graphics-probe/stage.py')
stage = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(stage)


class GraphicsStageTests(unittest.TestCase):
    def test_transitive_profiling_import_is_staged(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(stage, 'DEST', Path(directory)):
            stage.main()
            self.assertTrue((Path(directory) / 'graphics-metrics.js').is_file())

    def test_missing_import_fails_staging(self):
        copy = stage.shutil.copyfile
        def omit_metrics(source, target):
            if Path(source).name != 'graphics-metrics.js':
                return copy(source, target)
        with tempfile.TemporaryDirectory() as directory, patch.object(stage, 'DEST', Path(directory)), patch.object(stage.shutil, 'copyfile', omit_metrics):
            with self.assertRaisesRegex(RuntimeError, 'graphics-metrics.js'):
                stage.main()
