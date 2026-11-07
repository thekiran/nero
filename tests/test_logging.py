"""Exercise real CLI logging, persistence, bounded rotation and concurrent writers."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY=Path(sys.argv.pop(1)).resolve()
FIXTURE=Path(sys.argv.pop(1)).resolve()

class LoggingTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='nero_log_')
        self.addCleanup(self.temp.cleanup)
        self.path=Path(self.temp.name)/'nested'/'nero.log'
    def run_cli(self,commands,path=None):
        result=subprocess.run([str(BINARY)],input=commands.encode(),capture_output=True,
            env=dict(os.environ,NERO_LOG_FILE=str(path or self.path),NO_COLOR='1',TERM='dumb'),timeout=20)
        self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))
        return result.stdout.decode(errors='replace')
    def test_cli_commands_and_persistence(self):
        out=self.run_cli('wrong-command\nhelp wrong\nlog\nexit\n')
        self.assertIn('Unknown command rejected',out)
        self.assertIn('Invalid command parameters (input omitted)',out)
        self.assertNotIn('wrong-command',self.path.read_text())
        out=self.run_cli('log -errors\nlog -path\nexit\n')
        self.assertIn('[WARN] [cli]',out)
        self.assertIn(str(self.path),out)
        self.assertNotIn('[INFO] [session]',out)
        if os.name!='nt':
            self.assertEqual(self.path.stat().st_mode & 0o777,0o600)
    def test_clear_and_invalid_parameters(self):
        self.run_cli('wrong-command\nlog -clear\nlog -errors\nexit\n')
        self.assertNotIn('Unknown command rejected',self.path.read_text())
        out=self.run_cli('log -errors\nlog -clear\nlog -clear extra\nlog -errors\nexit\n')
        self.assertIn('No warnings or errors recorded.',out)
        self.assertIn('Invalid command parameters (input omitted)',out)
        self.assertFalse(Path(str(self.path)+'.1').exists())
    def test_memory_only(self):
        out=self.run_cli('wrong-command\nlog -errors\nexit\n',':memory:')
        self.assertIn('memory only',out)
        self.assertIn('[WARN] [cli]',out)
        self.assertFalse(self.path.exists())
    def test_unwritable_path_is_nonfatal(self):
        blocker=Path(self.temp.name)/'file'; blocker.write_text('not a directory')
        out=self.run_cli('wrong-command\nlog\nexit\n',blocker/'nero.log')
        self.assertIn('memory only; file unavailable',out)
        self.assertIn('Unknown command rejected',out)
    def fixture(self,mode):
        result=subprocess.run([str(FIXTURE),mode],capture_output=True,
            env=dict(os.environ,NERO_LOG_FILE=str(self.path)),timeout=60)
        self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))
        return result.stdout.decode(errors='replace')
    def test_control_characters_and_error_filter(self):
        out=self.fixture('errors')
        self.assertNotIn('info marker',out)
        self.assertIn('warning marker',out)
        self.assertIn('error marker ESC=',out)
        self.assertNotIn('\x1b',out)
        self.assertNotIn('\x1b',self.path.read_text())
    def test_rotation_and_concurrency(self):
        out=self.fixture('stress')
        lines=[s for s in out.splitlines() if '] [worker] ' in s]
        self.assertEqual(len(lines),256)
        self.assertEqual(len(set(lines)),256)
        for name in (self.path,Path(str(self.path)+'.1')):
            self.assertTrue(name.is_file())
            self.assertLessEqual(name.stat().st_size,1024*1024)
            for line in name.read_text().splitlines():
                self.assertRegex(line,r'^\[\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ\] \[(INFO|ERROR)\] \[worker\] worker=\d event=\d+ payload=')
        self.run_cli('log -clear\nexit\n')
        self.assertFalse(Path(str(self.path)+'.1').exists())

if __name__=='__main__': unittest.main()
