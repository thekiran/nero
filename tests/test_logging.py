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

    def test_partial_line_does_not_hide_next_error(self):
        self.path.parent.mkdir()
        self.path.write_text('[2000-01-01T00:00:00Z] [INFO] [seed] partial')
        self.fixture('first-error')
        lines=self.path.read_text().splitlines()
        self.assertEqual(len(lines),2)
        self.assertTrue(lines[1].startswith('['))
        out=self.run_cli('log -errors\nexit\n')
        self.assertIn('FIRST_ERROR_AFTER_PARTIAL_LINE',out)
    def test_clear_recovers_after_transient_write_failure(self):
        out=self.fixture('clear-after-failure')
        self.assertIn('clear_result=0',out)
        self.assertNotIn('OLD_PERSISTED_ERROR',self.path.read_text())
        out=self.run_cli('log -errors\nexit\n')
        self.assertNotIn('OLD_PERSISTED_ERROR',out)
        self.assertIn('No warnings or errors recorded.',out)
    def test_clear_reports_unresolved_storage_failure(self):
        out=self.fixture('clear-failure')
        self.assertIn('clear_result=-1',out)
        self.assertIn('OLD_PERSISTED_ERROR',out)
        self.assertIn('OLD_PERSISTED_ERROR',Path(str(self.path)+'.saved').read_text())
    def test_clear_backup_error_preserves_current_records(self):
        self.run_cli('wrong-command\nexit\n')
        old=self.path.read_text()
        backup=Path(str(self.path)+'.1'); backup.mkdir(); (backup/'keep').write_text('fixture')
        result=subprocess.run([str(BINARY)],input=b'log -clear\nexit\n',capture_output=True,
            env=dict(os.environ,NERO_LOG_FILE=str(self.path)),timeout=10)
        self.assertEqual(result.returncode,0)
        self.assertIn('Could not clear persisted logs',result.stderr.decode(errors='replace'))
        self.assertNotIn('Logs cleared.',result.stdout.decode(errors='replace'))
        self.assertEqual(self.path.read_text(),old)
    def test_output_failure_logs_actual_exit_status(self):
        with open(os.devnull,'rb') as read_only:
            result=subprocess.run([str(BINARY)],input=b'exit\n',stdout=read_only,stderr=subprocess.PIPE,
                env=dict(os.environ,NERO_LOG_FILE=str(self.path),TERM='dumb'),timeout=10)
        self.assertEqual(result.returncode,1)
        last=self.path.read_text().splitlines()[-1]
        self.assertIn('[ERROR] [session]',last)
        self.assertIn('status=1 stdout_error=1',last)
    @unittest.skipUnless(os.name=='posix','POSIX SIGPIPE fixture')
    def test_broken_output_pipe_logs_exit_status(self):
        read_fd,write_fd=os.pipe(); os.close(read_fd)
        try:
            result=subprocess.run([str(BINARY)],input=b'exit\n',stdout=write_fd,stderr=subprocess.PIPE,
                env=dict(os.environ,NERO_LOG_FILE=str(self.path)),timeout=10)
        finally: os.close(write_fd)
        status=128-result.returncode if result.returncode<0 else result.returncode
        self.assertEqual(status,141)
        last=self.path.read_text().splitlines()[-1]
        self.assertIn('[ERROR] [session]',last)
        self.assertIn('status=141 stdout_error=1',last)
    def test_nonregular_log_target_is_nonblocking(self):
        if os.name=='nt': target='NUL'
        elif hasattr(os,'mkfifo'):
            target=Path(self.temp.name)/'pipe.log'; os.mkfifo(target)
        else: self.skipTest('No special-file fixture on this platform')
        result=subprocess.run([str(BINARY)],input=b'log -path\nexit\n',capture_output=True,
            env=dict(os.environ,NERO_LOG_FILE=str(target)),timeout=3)
        self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))
        self.assertIn('memory only; file unavailable',result.stdout.decode(errors='replace'))
    def test_oversized_existing_files_are_bounded(self):
        self.path.parent.mkdir()
        record='[2000-01-01T00:00:00Z] [INFO] [seed] '+('x'*200)+'\n'
        for file,marker in ((self.path,'CURRENT_ERROR'),(Path(str(self.path)+'.1'),'BACKUP_ERROR')):
            file.write_text(record*9000+'[2000-01-01T00:00:00Z] [ERROR] [seed] '+marker+'\n')
        out=self.run_cli('log -errors\nexit\n')
        self.assertIn('CURRENT_ERROR',out)
        for file in (self.path,Path(str(self.path)+'.1')):
            self.assertLessEqual(file.stat().st_size,1024*1024)
        self.assertEqual(list(self.path.parent.glob('*.tmp.*')),[])

if __name__=='__main__': unittest.main()
