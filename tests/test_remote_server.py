"""Server boundary tests. No accounts, keys or model services are required."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('codex_gateway', ROOT / 'server/gateway.py')
gateway = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gateway)


class RemoteServerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.uid = os.getuid()
        self.workspace, self.state, self.config = gateway.account_paths(self.home, self.uid, 'demo')

    def save(self, name, content):
        path = self.config / name
        path.write_text(json.dumps(content))
        path.chmod(0o600)
        return path

    def test_private_project_and_state(self):
        self.assertEqual(self.workspace, self.home / 'workspaces/demo')
        self.assertEqual(self.state.stat().st_mode & 0o777, 0o700)
        self.assertEqual(self.workspace.stat().st_mode & 0o777, 0o700)

    def test_project_names_and_ssh_actions(self):
        options = gateway.parse_command('codex-cpp-remote --project demo --check')
        self.assertTrue(options.check)
        self.assertEqual(options.project, 'demo')
        self.assertTrue(gateway.parse_command('codex-cpp-remote --login').login)
        for command in ('id', 'bash', 'codex-cpp-remote --project ../bob', 'codex-cpp-remote --project "demo;id"'):
            with self.subTest(command=command), self.assertRaises(ValueError):
                gateway.parse_command(command)
        for command in ('codex-cpp-remote --worker demo', 'codex-cpp-remote --check --login', 'codex-cpp-remote --project demo --unsafe-execution'):
            with self.subTest(command=command), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                gateway.parse_command(command)

    def test_project_symlink_cannot_escape(self):
        (self.home / 'workspaces/escape').symlink_to(self.home.parent, target_is_directory=True)
        with self.assertRaises(ValueError):
            gateway.account_paths(self.home, self.uid, 'escape')

    def test_state_symlink_rejected(self):
        for path in (self.state,):
            path.rmdir()
            path.symlink_to(self.home.parent, target_is_directory=True)
        with self.assertRaises(ValueError):
            gateway.account_paths(self.home, self.uid, 'demo')

    def test_shared_home_rejected(self):
        self.home.chmod(0o755)
        with self.assertRaises(ValueError):
            gateway.account_paths(self.home, self.uid, 'demo')

    def test_other_uid_rejected(self):
        with self.assertRaises(ValueError):
            gateway.account_paths(self.home, self.uid + 1, 'demo')

    def test_defaults_are_per_account(self):
        args, secrets = gateway.agent_settings(self.config, self.uid)
        self.assertEqual(args, ['--provider', 'codex'])
        self.assertEqual(secrets, {})

    def test_configuration_and_secrets(self):
        self.save('config.json', {'provider': 'custom', 'base_url': 'http://127.0.0.1:1234', 'model': 'local', 'api_key_env': 'MY_API_KEY'})
        self.save('credentials.json', {'MY_API_KEY': 'fixture-only-key', 'OPENAI_API_KEY': 'fixture-openai-key'})
        args, secrets = gateway.agent_settings(self.config, self.uid)
        self.assertIn('--base-url', args)
        self.assertNotIn('fixture-only-key', ' '.join(args))
        self.assertEqual(secrets['MY_API_KEY'], 'fixture-only-key')

    def test_dangerous_or_incomplete_configuration_rejected(self):
        for config in ({'provider': 'custom'}, {'provider': 'unknown'}, {'unsafe_execution': True}, {'api_key_env': 'LD_PRELOAD'}, {'api_key_env': 'BASH_ENV'}, {'model': 'bad\nvalue'}):
            self.save('config.json', config)
            with self.subTest(config=config), self.assertRaises(ValueError):
                gateway.agent_settings(self.config, self.uid)

    def test_public_or_symlinked_credentials_rejected(self):
        path = self.save('credentials.json', {'OPENAI_API_KEY': 'fixture'})
        path.chmod(0o644)
        with self.assertRaises(ValueError):
            gateway.agent_settings(self.config, self.uid)
        path.unlink()
        target = self.home / 'other.json'
        target.write_text('{}')
        target.chmod(0o600)
        path.symlink_to(target)
        with self.assertRaises(ValueError):
            gateway.agent_settings(self.config, self.uid)

    def test_secrets_cannot_inject_process_environment(self):
        self.save('credentials.json', {'PATH': '/malicious', 'OPENAI_API_KEY': 'fixture'})
        with self.assertRaises(ValueError):
            gateway.agent_settings(self.config, self.uid)

    def test_client_environment_and_host_credentials_not_inherited(self):
        with patch.dict(os.environ, {'OPENAI_API_KEY': 'host-key', 'SSH_AUTH_SOCK': '/agent.sock', 'LD_PRELOAD': '/bad.so', 'TERM': 'xterm'}):
            environment = gateway.clean_environment(self.home, 'alice', {'OPENAI_API_KEY': 'alice-key'})
        self.assertEqual(environment['OPENAI_API_KEY'], 'alice-key')
        self.assertNotIn('SSH_AUTH_SOCK', environment)
        self.assertNotIn('LD_PRELOAD', environment)
        self.assertNotIn('host-key', environment.values())
        self.assertEqual(environment['HOME'], str(self.home))

    def test_interactive_start_fails_closed_without_sandbox(self):
        account = SimpleNamespace(pw_dir=str(self.home), pw_name='fixture')
        checks = {'agent': True, 'tmux': True, 'sandbox': False}
        with patch.object(gateway.os, 'getuid', return_value=self.uid or 1000), \
             patch.object(gateway.pwd, 'getpwuid', return_value=account), \
             patch.object(gateway, 'account_paths', return_value=(self.workspace, self.state, self.config)), \
             patch.object(gateway, 'agent_settings', return_value=(['--provider', 'codex'], {})), \
             patch.object(gateway.sys.stdin, 'isatty', return_value=True), \
             patch.object(gateway.sys.stdout, 'isatty', return_value=True), \
             patch.object(gateway, 'readiness', return_value=checks), \
             patch.object(gateway.os, 'execve') as launch, \
             patch.dict(os.environ, {'SSH_ORIGINAL_COMMAND': 'codex-cpp-remote --project demo'}):
            with self.assertRaises(ValueError):
                gateway.main()
            launch.assert_not_called()

    def test_failed_sandbox_is_not_ready(self):
        with patch.object(gateway.shutil, 'which', return_value='/fixture'), patch.object(gateway.subprocess, 'run', side_effect=OSError('unavailable')):
            checks = gateway.readiness({'PATH': '/usr/bin:/bin'})
        self.assertFalse(checks['sandbox'])
        self.assertFalse(all(checks.values()))


if __name__ == '__main__':
    unittest.main()
