"""Regression tests for the host-side consistency checker."""

import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest


CHECKER = pathlib.Path(__file__).with_name("check_consistency.py")


class ConfigSymbolTests(unittest.TestCase):
    def test_comments_are_ignored_but_code_is_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            tools = root / "tools"
            component = root / "components" / "example"
            main = root / "main"
            for path in (tools, component, main):
                path.mkdir(parents=True)
            shutil.copyfile(CHECKER, tools / CHECKER.name)
            (component / "CMakeLists.txt").write_text("")
            (main / "CMakeLists.txt").write_text("")
            source = component / "example.c"
            source.write_text(
                "// CONFIG_COMMENT_LINE\n"
                "/* CONFIG_COMMENT_BLOCK */\n"
                "const char *url = \"https://example.org/CONFIG_IN_STRING\";\n"
                "const char *marker = \"/* CONFIG_IN_QUOTED_COMMENT */\";\n"
                "int enabled = CONFIG_REAL;\n"
            )

            def check():
                return subprocess.run(
                    [sys.executable, str(tools / CHECKER.name)],
                    cwd=root, capture_output=True, text=True, check=False,
                )

            result = check()
            self.assertEqual(result.returncode, 1)
            self.assertIn("CONFIG_REAL used in components/example/example.c", result.stdout)
            self.assertNotIn("CONFIG_COMMENT_LINE", result.stdout)
            self.assertNotIn("CONFIG_COMMENT_BLOCK", result.stdout)
            self.assertNotIn("CONFIG_IN_STRING", result.stdout)
            self.assertNotIn("CONFIG_IN_QUOTED_COMMENT", result.stdout)

            (component / "Kconfig").write_text("config REAL\n    bool \"Real\"\n")
            result = check()
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("0 problem(s)", result.stdout)


if __name__ == "__main__":
    unittest.main()
