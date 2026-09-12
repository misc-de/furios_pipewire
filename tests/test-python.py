#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
"""The parts written in Python, and the seam between the app and audioctl.

That seam is the interesting one. The app reads audioctl's output line by line
and looks for labels; renaming a label in audioctl - which happened while
translating the project to English - silently breaks the app, and nothing
notices until someone opens it and sees "unknown".
"""
import importlib.util
import os
import io
import re
import subprocess as subprocess_real
import sys
import types
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests"))

import gi_stub  # noqa: E402  - has to come before anything that imports gi

recorder = gi_stub.install()


def load(path, name):
    """Import a script by path, so its lines are the ones being measured.

    Running it as a subprocess would be simpler and would tell the coverage
    tracer nothing - it only sees what happens in this process.
    """
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gen = load(ROOT / "gen-pipewire-hal-conf.py", "gen_hal_conf")
watcher = load(ROOT / "tools" / "furios-audio-pause-on-disconnect.py", "watcher")
switcher = load(ROOT / "gui" / "furios-audio-switch.py", "switcher")
sco = load(ROOT / "tools" / "furios-audio-sco-hold.py", "sco_hold")
hands_free_sink_real = sco.hands_free_sink


class ConfigGenerator(unittest.TestCase):
    """gen-pipewire-hal-conf.py rewrites FuriOS' own config."""

    def setUp(self):
        self.template = (
            "# Daemon config file\n"
            "context.properties = {\n"
            "    default.clock.rate = 48000\n"
            "}\n"
            "context.spa-libs = {\n"
            "    audio.convert.* = audioconvert/libspa-audioconvert\n"
            "}\n"
            "context.objects = [\n"
            "]\n"
        )

    def run_gen(self, text, tmp):
        """Call main() in this process - a subprocess would not be measured."""
        src, dst = tmp / "in.conf", tmp / "out.conf"
        src.write_text(text)
        out, err = io.StringIO(), io.StringIO()
        argv = sys.argv
        sys.argv = ["gen-pipewire-hal-conf.py", str(src), str(dst)]
        try:
            with redirect_stdout(out), redirect_stderr(err):
                code = gen.main()
        finally:
            sys.argv = argv
        return type("Result", (), {"returncode": code, "stderr": err.getvalue(),
                                   "stdout": out.getvalue()}), dst

    def test_registers_the_plugin(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            res, dst = self.run_gen(self.template, Path(d))
            self.assertEqual(res.returncode, 0, res.stderr)
            out = dst.read_text()
            self.assertIn("api.droid.*", out)
            self.assertIn("droid/libspa-droid", out)

    def test_keeps_what_furios_had(self):
        """The point of generating instead of copying: their changes survive."""
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            _, dst = self.run_gen(self.template, Path(d))
            out = dst.read_text()
            self.assertIn("audio.convert.*", out)
            self.assertIn("default.clock.rate = 48000", out)

    def test_says_so_when_the_template_changed(self):
        """A silent pass on a template it did not understand would be worse."""
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            res, _ = self.run_gen("nothing we recognise\n", Path(d))
            self.assertNotEqual(res.returncode, 0)
            self.assertIn("context.spa-libs", res.stderr)

    def test_it_notices_a_template_that_already_has_the_plugin(self):
        """Running it twice must not register the library a second time."""
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            already = self.template.replace(
                "context.spa-libs = {\n",
                "context.spa-libs = {\n    api.droid.* = droid/libspa-droid\n")
            res, dst = self.run_gen(already, Path(d))
            self.assertEqual(res.returncode, 0)
            self.assertIn("already in the template", res.stderr)
            self.assertEqual(dst.read_text().count("api.droid.*"), 1)

    def test_it_says_so_when_the_objects_block_is_gone(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            without = self.template.replace("context.objects = [\n]\n", "")
            res, _ = self.run_gen(without, Path(d))
            self.assertNotEqual(res.returncode, 0)
            self.assertIn("context.objects", res.stderr)

    def test_marks_the_file_as_generated(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            _, dst = self.run_gen(self.template, Path(d))
            head = dst.read_text().splitlines()[0]
            self.assertIn("Generated by", head)


class AppReadsAudioctl(unittest.TestCase):
    """The switcher app parses audioctl's status output."""

    @classmethod
    def setUpClass(cls):
        sys.modules.setdefault("gi", None)
        cls.audioctl = (ROOT / "audioctl").read_text()
        cls.app = (ROOT / "gui" / "furios-audio-switch.py").read_text()

    def labels_in_app(self):
        return re.findall(r'line\.startswith\("([^"]+)"\)', self.app)

    def test_every_label_the_app_looks_for_is_one_audioctl_prints(self):
        for label in self.labels_in_app():
            with self.subTest(label=label):
                self.assertIn(label, self.audioctl,
                              "the app waits for a line audioctl never prints")

    def test_the_app_looks_for_something_at_all(self):
        """Guards the test above from passing by finding nothing."""
        self.assertGreaterEqual(len(self.labels_in_app()), 4)

    def test_test_mode_is_recognised_by_the_word_audioctl_uses(self):
        self.assertIn('startswith("yes")', self.app)
        self.assertRegex(self.audioctl, r"Test mode:\s+yes")

    def test_the_dmnr_switch_speaks_the_words_the_script_understands(self):
        script = (ROOT / "experiments" / "dmnr-handsfree.sh").read_text()
        self.assertIn('"on" if row.get_active() else "off"', self.app)
        # The script dispatches on these exact words; the app sends them.
        for word in ("on)", "off)"):
            self.assertRegex(script, r"(?m)^%s$" % re.escape(word))

    def test_the_machine_readable_state_line_matches(self):
        script = (ROOT / "experiments" / "dmnr-handsfree.sh").read_text()
        self.assertIn("state=on", script)
        self.assertIn('"state=on" in out', self.app)


class FakeBus:
    """A D-Bus connection that answers what a test tells it to.

    The watcher asks two things: who is on the bus, and what each MPRIS player
    is doing. Everything else it does is call Pause, which is recorded.
    """

    def __init__(self, names=(), status=None, fail_on=None):
        self.names = list(names)
        self.status = status or {}
        self.fail_on = fail_on or set()
        self.paused = []
        self.subscriptions = []

    def call_sync(self, dest, path, iface, method, args, reply, flags,
                  timeout, cancellable):
        if method == "ListNames":
            if "ListNames" in self.fail_on:
                raise watcher.GLib.Error("no bus today")
            return FakeVariant([self.names])
        if method == "Get":
            if dest in self.fail_on:
                raise watcher.GLib.Error("player went away")
            return FakeVariant([self.status.get(dest, "Stopped")])
        if method == "Pause":
            if dest in self.fail_on:
                raise watcher.GLib.Error("will not pause")
            self.paused.append(dest)
            return None
        raise AssertionError("unexpected call: %s" % method)

    def signal_subscribe(self, *args):
        self.subscriptions.append(args)
        return 1


class BluezBus:
    """A system bus that answers what a Bluetooth device is.

    The watcher asks BlueZ for the device's UUIDs before it pauses anything,
    so that a watch or a keyboard dropping off does not stop the music.
    """

    def __init__(self, uuids=None, fail=False):
        self.uuids = uuids
        self.fail = fail
        self.subscriptions = []

    def call_sync(self, dest, path, iface, method, args, reply, flags,
                  timeout, cancellable):
        if method != "Get":
            raise AssertionError("unexpected call: %s" % method)
        if self.fail:
            raise watcher.GLib.Error("no such device")
        return FakeVariant([self.uuids])

    def signal_subscribe(self, *args):
        self.subscriptions.append(args)
        return 1


class FakeVariant:
    def __init__(self, value):
        self.value = value

    def unpack(self):
        return self.value


class SwitcherWords(unittest.TestCase):
    """What the app puts in front of someone who is not reading code.

    This is the part where being wrong is invisible: a label that says the
    opposite of what is happening reads as a bug in the audio, and the person
    goes looking in the wrong place. It happened once already - the app showed
    "PipeWire holds the HAL" above "Sound server: PulseAudio", which is not a
    contradiction but reads exactly like one.
    """

    def test_pipewires_pulse_interface_is_explained_rather_than_quoted(self):
        said = switcher.server_in_words("PulseAudio (on PipeWire 1.6.6)")
        self.assertIn("PipeWire", said)
        self.assertIn("1.6.6", said)
        self.assertIn("older apps", said)

    def test_a_version_it_cannot_find_still_reads_sensibly(self):
        said = switcher.server_in_words("PulseAudio (on PipeWire)")
        self.assertIn("PipeWire", said)

    def test_the_shipped_server_is_named_as_such(self):
        self.assertIn("shipped", switcher.server_in_words("pulseaudio"))
        self.assertIn("shipped", switcher.server_in_words("PulseAudio"))

    def test_nothing_at_all_is_not_dressed_up(self):
        self.assertEqual("not reachable", switcher.server_in_words(""))
        self.assertEqual("not reachable", switcher.server_in_words("-"))
        self.assertEqual("not reachable", switcher.server_in_words(None))

    def test_something_unexpected_is_passed_through_unchanged(self):
        self.assertEqual("jackd", switcher.server_in_words("jackd"))


class FakeProcess:
    """A Gio.Subprocess that hands back what a test wrote for it.

    audioctl takes up to fifteen seconds and the window must not freeze, so the
    app reads its output line by line as it arrives. That is the part worth
    testing: not that a subprocess runs, but that the lines are assembled and
    the end is noticed.
    """

    def __init__(self, lines=(), ok=True, fail_at=None):
        self.lines = list(lines)
        self.ok = ok
        self.fail_at = fail_at
        self.waited = False

    def get_stdout_pipe(self):
        return self

    def get_successful(self):
        return self.ok

    def communicate_utf8_finish(self, _res):
        if self.fail_at == "communicate":
            raise switcher.GLib.Error("pipe broke")
        return True, "\n".join(self.lines), None

    def communicate_utf8_async(self, _stdin, _cancellable, callback):
        callback(self, None)

    def wait_async(self, _cancellable, callback):
        callback(self, None)

    def wait_finish(self, _res):
        self.waited = True
        if self.fail_at == "wait":
            raise switcher.GLib.Error("never finished")


class FakeStream:
    """Gio.DataInputStream over the lines of a FakeProcess."""

    def __init__(self, process):
        self.process = process
        self.index = 0

    def read_line_async(self, _priority, _cancellable, callback):
        callback(self, None)

    def read_line_finish_utf8(self, _res):
        if self.process.fail_at == "read":
            raise switcher.GLib.Error("stream died")
        if self.index >= len(self.process.lines):
            return None, 0
        line = self.process.lines[self.index]
        self.index += 1
        return line, len(line)


class RunsAudioctl(unittest.TestCase):
    """How the app talks to audioctl."""

    def setUp(self):
        self.process = None
        self.original_new = switcher.Gio.Subprocess.new
        self.original_stream = switcher.Gio.DataInputStream.new

    def tearDown(self):
        switcher.Gio.Subprocess.new = self.original_new
        switcher.Gio.DataInputStream.new = self.original_stream

    def arrange(self, **kwargs):
        process = FakeProcess(**kwargs)
        switcher.Gio.Subprocess.new = lambda *a, **k: process
        switcher.Gio.DataInputStream.new = lambda pipe: FakeStream(process)
        return process

    def test_output_is_collected_and_handed_over_at_the_end(self):
        self.arrange(lines=["one", "two"])
        seen = []
        switcher.run_async(["audioctl", "status"], lambda ok, out: seen.append((ok, out)))
        self.assertEqual(seen, [(True, "one\ntwo")])

    def test_with_a_line_callback_the_lines_arrive_as_they_come(self):
        self.arrange(lines=["step one", "step two", ""])
        lines, done = [], []
        switcher.run_async(["audioctl", "set", "pw-hal"],
                           lambda ok, out: done.append((ok, out)),
                           on_line=lines.append)
        self.assertEqual(lines, ["step one", "step two"])
        self.assertEqual(done, [(True, "step one\nstep two")])

    def test_a_command_that_fails_is_reported_as_such(self):
        self.arrange(lines=["went wrong"], ok=False)
        seen = []
        switcher.run_async(["audioctl", "status"], lambda ok, out: seen.append((ok, out)))
        self.assertEqual(seen[0][0], False)

    def test_a_command_that_will_not_start_is_reported(self):
        def refuse(*_a, **_k):
            raise switcher.GLib.Error("no such file")
        switcher.Gio.Subprocess.new = refuse
        seen = []
        switcher.run_async(["nothing"], lambda ok, out: seen.append((ok, out)))
        self.assertEqual(seen[0][0], False)
        self.assertIn("no such file", seen[0][1])

    def test_a_pipe_that_breaks_while_reading_is_reported(self):
        self.arrange(lines=["a"], fail_at="read")
        seen = []
        switcher.run_async(["audioctl"], lambda ok, out: seen.append((ok, out)),
                           on_line=lambda _l: None)
        self.assertEqual(seen[0][0], False)

    def test_a_process_that_never_finishes_is_reported(self):
        self.arrange(lines=[], fail_at="wait")
        seen = []
        switcher.run_async(["audioctl"], lambda ok, out: seen.append((ok, out)),
                           on_line=lambda _l: None)
        self.assertEqual(seen[0][0], False)

    def test_a_broken_pipe_without_a_line_callback_is_reported(self):
        self.arrange(lines=["x"], fail_at="communicate")
        seen = []
        switcher.run_async(["audioctl"], lambda ok, out: seen.append((ok, out)))
        self.assertEqual(seen[0][0], False)


class Recording:
    """A widget that remembers what it was told, and answers what a test set."""

    def __init__(self, active=False):
        self.subtitle = None
        self.sensitive = True
        self.active = active
        self.text = None
        self.fraction = None
        self.revealed = None

    def set_subtitle(self, text):
        self.subtitle = text

    def set_sensitive(self, value):
        self.sensitive = value

    def set_active(self, value):
        self.active = value

    def get_active(self):
        return self.active

    def set_text(self, text):
        self.text = text

    def set_fraction(self, value):
        self.fraction = value

    def pulse(self):
        self.fraction = "pulsing"

    def set_reveal_child(self, value):
        self.revealed = value

    def add_toast(self, toast):
        # The stub keeps constructor arguments as attributes, so the title of
        # the toast is readable rather than the object's name.
        self.text = getattr(toast, "title", toast)


class FindsItsTools(unittest.TestCase):
    """Which "audioctl" the app starts.

    The installed paths come before $PATH, because what is started here used to
    go on and ask polkit for root - and letting the search order decide which
    binary that is was the one thing not to do. The reason is gone, the order
    stays: $PATH is the last resort, not the first.
    """

    def test_an_installed_path_wins_over_the_search_path(self):
        original = switcher.os.access
        switcher.os.access = lambda path, mode: path == "/usr/bin/audioctl"
        try:
            self.assertEqual("/usr/bin/audioctl", switcher._tool("audioctl"))
        finally:
            switcher.os.access = original

    def test_usr_local_wins_over_usr(self):
        original = switcher.os.access
        switcher.os.access = lambda path, mode: True
        try:
            self.assertEqual("/usr/local/bin/audioctl", switcher._tool("audioctl"))
        finally:
            switcher.os.access = original

    def test_with_nothing_installed_it_falls_back(self):
        original_access, original_which = switcher.os.access, switcher.shutil.which
        switcher.os.access = lambda path, mode: False
        switcher.shutil.which = lambda name: "/opt/bin/" + name
        try:
            self.assertEqual("/opt/bin/audioctl", switcher._tool("audioctl"))
        finally:
            switcher.os.access, switcher.shutil.which = original_access, original_which


class TheWindow(unittest.TestCase):
    """The window, driven through its own callbacks.

    Building it needs a stub, and a stub proves nothing about GTK. What it does
    prove is the part that has been wrong before: which words end up in front
    of someone, and whether the switch follows the state or fights it.
    """

    def setUp(self):
        self.win = switcher.Window(switcher.Adw.Application())
        for name in ("row_profile", "row_server", "row_sinks", "switch_row",
                     "persist_row", "dmnr_row", "refresh_btn", "progress",
                     "progress_revealer", "toasts"):
            setattr(self.win, name, Recording())
        self.ran = []
        self.original = switcher.run_async
        switcher.run_async = lambda argv, done, on_line=None: self.ran.append(
            (argv, done, on_line))

    def tearDown(self):
        switcher.run_async = self.original

    STATUS = ("Profile (active):   pw-hal\n"
              "Profile (persistent): standard\n"
              "Test mode:          yes - falls back on reboot\n"
              "Pulse server:       PulseAudio (on PipeWire 1.6.6)\n"
              "Sinks:              droid-sink,droid-voip-sink\n")

    def test_status_is_turned_into_something_a_person_can_read(self):
        self.win.on_status(True, self.STATUS)
        self.assertIn("PipeWire owns the HAL", self.win.row_profile.subtitle)
        self.assertIn("until reboot", self.win.row_profile.subtitle)
        self.assertIn("older apps", self.win.row_server.subtitle)
        self.assertEqual("droid-sink, droid-voip-sink", self.win.row_sinks.subtitle)
        self.assertTrue(self.win.switch_row.active)

    def test_the_shipped_state_is_named_as_such(self):
        self.win.on_status(True, "Profile (active):   standard\nSinks:              x\n")
        self.assertIn("as shipped", self.win.row_profile.subtitle)
        self.assertFalse(self.win.switch_row.active)

    def test_the_tunnel_profile_has_its_own_sentence(self):
        self.win.on_status(True, "Profile (active):   pw-tunnel\n")
        self.assertIn("PipeWire gets a sink", self.win.row_profile.subtitle)

    def test_a_profile_it_does_not_know_is_shown_as_it_is(self):
        self.win.on_status(True, "Profile (active):   something-else\n")
        self.assertIn("something-else", self.win.row_profile.subtitle)

    def test_a_warning_from_audioctl_is_passed_on(self):
        self.win.on_status(True, self.STATUS +
                           "WARNING:            recorded is \"standard\"\n")
        self.assertIn("recorded is", self.win.row_profile.subtitle)

    def test_no_sinks_reads_as_none_rather_than_as_a_dash(self):
        self.win.on_status(True, "Profile (active):   standard\nSinks:\n")
        self.assertEqual("none", self.win.row_sinks.subtitle)

    def test_the_switch_does_not_fire_while_it_is_being_synced(self):
        """Following the state must not look like someone flipping it."""
        self.win.on_status(True, self.STATUS)
        self.assertEqual([], self.ran, "syncing the switch started a command")

    def test_flipping_the_switch_on_asks_for_pw_hal(self):
        self.win.switch_row.active = True
        self.win.persist_row.active = False
        self.win.on_switch(self.win.switch_row, None)
        argv = self.ran[0][0]
        self.assertEqual(["try", "pw-hal"], argv[1:])

    def test_and_with_remember_ticked_it_asks_for_set(self):
        self.win.switch_row.active = True
        self.win.persist_row.active = True
        self.win.on_switch(self.win.switch_row, None)
        self.assertEqual(["set", "pw-hal"], self.ran[0][0][1:])

    def test_flipping_it_off_always_sets_standard_persistently(self):
        """Off means off after a reboot too - a test-mode "off" would come back."""
        self.win.switch_row.active = False
        self.win.on_switch(self.win.switch_row, None)
        self.assertEqual(["set", "standard"], self.ran[0][0][1:])

    def test_the_switch_is_ignored_while_something_is_running(self):
        self.win.busy = True
        self.win.on_switch(self.win.switch_row, None)
        self.assertEqual([], self.ran)

    def test_progress_shows_what_audioctl_says_and_cuts_it_to_a_line(self):
        self.win.on_progress_line("x" * 200)
        self.assertEqual(60, len(self.win.progress.text))

    def test_a_finished_switch_shows_the_last_thing_it_said(self):
        self.win.on_switched(True, "step\nplease check telephony\n")
        self.assertIn("check telephony", str(self.win.toasts.text))

    def test_a_switch_with_no_output_still_says_something(self):
        self.win.on_switched(True, "")
        self.assertIsNotNone(self.win.toasts.text)

    def test_a_failed_switch_says_so_and_shows_the_output(self):
        self.win.on_switched(False, "it went wrong")
        self.assertIn("failed", str(self.win.toasts.text).lower())

    def test_a_failed_switch_without_output_still_reports(self):
        self.win.on_switched(False, "")
        self.assertIsNotNone(self.win.toasts.text)

    def test_the_echo_switch_asks_the_dmnr_helper(self):
        self.win.dmnr_row.active = True
        self.win.on_dmnr(self.win.dmnr_row, None)
        self.assertEqual("on", self.ran[0][0][1])
        self.ran.clear()
        self.win.busy = False
        self.win.dmnr_row.active = False
        self.win.on_dmnr(self.win.dmnr_row, None)
        self.assertEqual("off", self.ran[0][0][1])

    def test_the_echo_switch_is_ignored_while_busy(self):
        self.win.busy = True
        self.win.on_dmnr(self.win.dmnr_row, None)
        self.assertEqual([], self.ran)

    def test_the_echo_result_is_reported_either_way(self):
        self.win.on_dmnr_done(True, "state=on")
        self.assertIn("Echo suppression", str(self.win.toasts.text))
        self.win.on_dmnr_done(False, "")
        self.assertIn("Could not", str(self.win.toasts.text))

    def test_a_device_without_the_helper_disables_the_switch(self):
        self.win.on_dmnr_status(False, "")
        self.assertFalse(self.win.dmnr_row.sensitive)
        self.assertIn("not available", self.win.dmnr_row.subtitle)

    def test_the_echo_switch_follows_the_state_of_the_file(self):
        self.win.on_dmnr_status(True, "state=on\nfile: ...\n")
        self.assertTrue(self.win.dmnr_row.active)
        self.assertIn("modified tuning file", self.win.dmnr_row.subtitle)
        self.win.on_dmnr_status(True, "state=off\n")
        self.assertFalse(self.win.dmnr_row.active)
        self.assertIn("Vendor setting", self.win.dmnr_row.subtitle)

    def test_restore_sound_runs_the_same_rescue_as_the_command_line(self):
        self.win.on_rescue(None)
        self.assertEqual("rescue", self.ran[0][0][1])

    def test_restore_is_ignored_while_busy(self):
        self.win.busy = True
        self.win.on_rescue(None)
        self.assertEqual([], self.ran)

    def test_a_finished_restore_says_what_it_did(self):
        self.win.on_rescued(True, "")
        self.assertIn("65 %", str(self.win.toasts.text))
        self.win.on_rescued(False, "no")
        self.assertIn("failed", str(self.win.toasts.text).lower())

    def test_refresh_asks_both_helpers(self):
        self.win.refresh()
        self.assertEqual(2, len(self.ran))

    def test_busy_says_what_is_happening_and_locks_the_controls(self):
        self.win.set_busy(True)
        self.assertFalse(self.win.switch_row.sensitive)
        self.assertIn("takes a moment", self.win.switch_row.subtitle)
        self.win.switch_row.active = True
        self.win.set_busy(False)
        self.assertTrue(self.win.switch_row.sensitive)
        self.assertIn("talks to the HAL", self.win.switch_row.subtitle)
        self.win.switch_row.active = False
        self.win.set_busy(False)
        self.assertIn("as shipped", self.win.switch_row.subtitle)

    def test_the_progress_bar_pulses_rather_than_inventing_a_percentage(self):
        self.win.pulse_start("Switching …")
        self.assertEqual("Switching …", self.win.progress.text)
        self.assertEqual("pulsing", self.win.progress.fraction)
        self.assertTrue(self.win.progress_revealer.revealed)
        self.assertTrue(self.win._pulse_tick())
        self.win.pulse_stop()
        self.assertFalse(self.win.progress_revealer.revealed)

    def test_the_application_opens_a_window(self):
        """Starting up, which is all there is to it now.

        This used to put a polkit authentication agent in place, because a
        switch needed root and phosh registers none of its own. audioctl keeps
        the profile under $HOME these days, so there is nothing to authenticate
        - and an app that handles no passwords cannot mishandle them.
        """
        app = switcher.App()
        app.props.active_window = None
        app.do_activate()



class PauseOnDisconnect(unittest.TestCase):
    """The watcher that keeps music off the loudspeaker.

    Earbuds run out of battery mid-track; without this the audio moves to the
    loudspeaker of a phone that may be in someone's pocket, in a room with
    other people in it.
    """

    def test_a_player_that_is_playing_is_paused(self):
        bus = FakeBus(names=["org.mpris.MediaPlayer2.emilia", "org.freedesktop.DBus"],
                      status={"org.mpris.MediaPlayer2.emilia": "Playing"})
        with redirect_stdout(io.StringIO()):
            watcher.pause_all(bus)
        self.assertEqual(bus.paused, ["org.mpris.MediaPlayer2.emilia"])

    def test_a_player_that_is_paused_is_left_alone(self):
        bus = FakeBus(names=["org.mpris.MediaPlayer2.emilia"],
                      status={"org.mpris.MediaPlayer2.emilia": "Paused"})
        with redirect_stdout(io.StringIO()):
            watcher.pause_all(bus)
        self.assertEqual(bus.paused, [])

    def test_names_that_are_not_players_are_ignored(self):
        bus = FakeBus(names=["org.freedesktop.DBus", "org.bluez"],
                      status={})
        with redirect_stdout(io.StringIO()):
            watcher.pause_all(bus)
        self.assertEqual(bus.paused, [])

    def test_several_players_are_all_paused(self):
        names = ["org.mpris.MediaPlayer2.a", "org.mpris.MediaPlayer2.b"]
        bus = FakeBus(names=names, status={n: "Playing" for n in names})
        with redirect_stdout(io.StringIO()):
            watcher.pause_all(bus)
        self.assertEqual(sorted(bus.paused), sorted(names))

    def test_a_player_that_will_not_answer_is_skipped(self):
        """A player can disappear between being listed and being asked."""
        bus = FakeBus(names=["org.mpris.MediaPlayer2.gone"],
                      status={}, fail_on={"org.mpris.MediaPlayer2.gone"})
        with redirect_stdout(io.StringIO()):
            watcher.pause_all(bus)
        self.assertEqual(bus.paused, [])

    def test_a_player_that_refuses_to_pause_is_reported(self):
        bus = FakeBus(names=["org.mpris.MediaPlayer2.stubborn"],
                      status={"org.mpris.MediaPlayer2.stubborn": "Playing"})
        bus.fail_on = {"org.mpris.MediaPlayer2.stubborn"}
        bus.status = {"org.mpris.MediaPlayer2.stubborn": "Playing"}

        # Asking for its status has to succeed, only the pause fails.
        original = bus.call_sync

        def only_pause_fails(dest, path, iface, method, *rest):
            if method == "Pause":
                raise watcher.GLib.Error("will not pause")
            bus.fail_on = set()
            try:
                return original(dest, path, iface, method, *rest)
            finally:
                bus.fail_on = {"org.mpris.MediaPlayer2.stubborn"}

        bus.call_sync = only_pause_fails
        out = io.StringIO()
        with redirect_stdout(out):
            watcher.pause_all(bus)
        self.assertIn("could not pause", out.getvalue())

    def test_a_bus_that_will_not_be_listed_gives_nothing(self):
        bus = FakeBus(fail_on={"ListNames"})
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(watcher.playing_players(bus), [])
        self.assertIn("could not list bus names", out.getvalue())

    def test_nothing_playing_says_so_rather_than_nothing(self):
        bus = FakeBus(names=[])
        out = io.StringIO()
        with redirect_stdout(out):
            watcher.pause_all(bus)
        self.assertIn("nothing was playing", out.getvalue())

    def test_a_player_that_is_only_readable_later_is_still_caught(self):
        """The morning this service was written for, and then failed at.

        Earbuds died at 06:31:30 with a podcast playing. The one look at the
        moment of the signal found nothing, the podcast moved to the
        loudspeaker and played for 66 minutes. Reproduced with the service
        stopped: the player really does stay "Playing" and follows the stream
        onto the speaker. So a look that comes up empty is not an answer - it
        has to be asked again.
        """
        bus = FakeBus(names=["org.mpris.MediaPlayer2.emilia"],
                      status={"org.mpris.MediaPlayer2.emilia": "Paused"})
        retry = watcher.Retry(bus)

        with redirect_stdout(io.StringIO()):
            self.assertTrue(retry.tick(), "it has to keep looking")
        self.assertEqual(bus.paused, [], "nothing was playing on that look")

        # A moment later the player is readable again, and playing.
        bus.status["org.mpris.MediaPlayer2.emilia"] = "Playing"
        with redirect_stdout(io.StringIO()):
            retry.tick()
        self.assertEqual(bus.paused, ["org.mpris.MediaPlayer2.emilia"])

    def test_the_retries_run_out(self):
        """It watches for a few seconds, not forever."""
        bus = FakeBus(names=[])
        retry = watcher.Retry(bus)
        with redirect_stdout(io.StringIO()):
            ticks = 1
            while retry.tick():
                ticks += 1
                self.assertLess(ticks, 100, "this should not go on forever")
        self.assertEqual(ticks, len(watcher.RETRY_DELAYS_MS))

    def test_a_second_disconnect_calls_off_the_first_watch(self):
        """Two devices dropping in a row, or one dropping twice.

        Each disconnect starts its own watch; the one before it has to be
        called off, or the old one keeps pausing on a state it no longer knows
        anything about.
        """
        buses = []

        def bus_get_sync(kind, _cancellable):
            bus = FakeBus(names=["org.mpris.MediaPlayer2.emilia"],
                          status={"org.mpris.MediaPlayer2.emilia": "Playing"})
            buses.append(bus)
            return bus

        original_get, original_loop = watcher.Gio.bus_get_sync, watcher.GLib.MainLoop
        original_timeout = watcher.GLib.timeout_add
        scheduled = []
        watcher.Gio.bus_get_sync = bus_get_sync
        watcher.GLib.MainLoop = lambda: type("L", (), {"run": lambda self: None})()
        watcher.GLib.timeout_add = lambda _ms, fn: scheduled.append(fn)
        try:
            with redirect_stdout(io.StringIO()):
                watcher.main()
            system, session = buses[0], buses[1]
            handler = system.subscriptions[0][-1]
            lost = FakeVariant(["org.bluez.Device1", {"Connected": False}, []])

            with redirect_stdout(io.StringIO()):
                handler(None, None, "/org/bluez/hci0/dev_AA", None, None, lost)
            first_round = list(scheduled)
            with redirect_stdout(io.StringIO()):
                handler(None, None, "/org/bluez/hci0/dev_BB", None, None, lost)

            session.paused = []
            with redirect_stdout(io.StringIO()):
                for fire in first_round:
                    fire()
            self.assertEqual(session.paused, [],
                             "the watch from the first disconnect must be off")
        finally:
            watcher.Gio.bus_get_sync = original_get
            watcher.GLib.MainLoop = original_loop
            watcher.GLib.timeout_add = original_timeout

    def test_a_cancelled_retry_does_nothing(self):
        bus = FakeBus(names=["org.mpris.MediaPlayer2.emilia"],
                      status={"org.mpris.MediaPlayer2.emilia": "Playing"})
        retry = watcher.Retry(bus)
        retry.cancel()
        with redirect_stdout(io.StringIO()):
            self.assertFalse(retry.tick())
        self.assertEqual(bus.paused, [], "a cancelled retry must not pause")

    def test_a_player_too_slow_to_answer_is_reported(self):
        """Silence here is what let a podcast run all morning.

        A player that fails to answer within the timeout looked exactly like
        one that was not playing, and the code said nothing at all about it.
        """
        bus = FakeBus(names=["org.mpris.MediaPlayer2.slow"],
                      status={}, fail_on={"org.mpris.MediaPlayer2.slow"})
        out = io.StringIO()
        with redirect_stdout(out):
            watcher.playing_players(bus)
        self.assertIn("could not ask slow", out.getvalue())

    def test_it_watches_bluez_and_reacts_to_a_lost_connection(self):
        """main() wires the signal up; the handler is what decides."""
        buses = []

        def bus_get_sync(kind, _cancellable):
            bus = FakeBus(names=["org.mpris.MediaPlayer2.emilia"],
                          status={"org.mpris.MediaPlayer2.emilia": "Playing"})
            buses.append(bus)
            return bus

        original_get, original_loop = watcher.Gio.bus_get_sync, watcher.GLib.MainLoop
        original_timeout = watcher.GLib.timeout_add
        ran = []
        scheduled = []
        watcher.Gio.bus_get_sync = bus_get_sync
        watcher.GLib.MainLoop = lambda: type(
            "Loop", (), {"run": lambda self: ran.append(True)})()
        # The retries are timeouts on the main loop; there is no loop here, so
        # keep them and fire them by hand.
        watcher.GLib.timeout_add = lambda _ms, fn: scheduled.append(fn)
        try:
            out = io.StringIO()
            with redirect_stdout(out):
                watcher.main()
            self.assertIn("watching BlueZ", out.getvalue())
            self.assertTrue(ran, "it has to keep running, not return at once")

            system, session = buses[0], buses[1]
            self.assertEqual(len(system.subscriptions), 1)
            handler = system.subscriptions[0][-1]

            # A Bluetooth device losing its connection: pause whatever plays.
            with redirect_stdout(out):
                handler(None, None, "/org/bluez/hci0/dev_AA", None, None,
                        FakeVariant(["org.bluez.Device1", {"Connected": False},
                                     []]))
            self.assertEqual(session.paused, ["org.mpris.MediaPlayer2.emilia"])
            self.assertIn("dev_AA disconnected", out.getvalue())

            # Losing a device also arms the retries.
            self.assertEqual(len(scheduled), len(watcher.RETRY_DELAYS_MS),
                             "one timeout per retry delay")

            # Connecting is not our business - and it calls the retries off.
            # Somebody just put their earbuds back in; pausing their music a
            # few seconds later would be worse than the problem this service
            # exists for.
            session.paused = []
            with redirect_stdout(out):
                handler(None, None, "/org/bluez/hci0/dev_AA", None, None,
                        FakeVariant(["org.bluez.Device1", {"Connected": True},
                                     []]))
            self.assertEqual(session.paused, [])

            with redirect_stdout(out):
                for fire in scheduled:
                    fire()
            self.assertEqual(session.paused, [],
                             "a retry after reconnecting must stay silent")

            # Neither is anything that is not a Bluetooth device.
            with redirect_stdout(out):
                handler(None, None, "/org/bluez/hci0/dev_AA", None, None,
                        FakeVariant(["org.bluez.Adapter1", {"Powered": False},
                                     []]))
            self.assertEqual(session.paused, [])
        finally:
            watcher.Gio.bus_get_sync = original_get
            watcher.GLib.MainLoop = original_loop
            watcher.GLib.timeout_add = original_timeout

    # --- which devices are worth pausing for ------------------------------

    def test_earbuds_count_as_audio(self):
        bus = BluezBus(["0000110b-0000-1000-8000-00805f9b34fb",
                        "0000111e-0000-1000-8000-00805f9b34fb"])
        self.assertTrue(watcher.carries_audio(bus, "/org/bluez/hci0/dev_AA"))

    def test_a_watch_does_not(self):
        """A smartwatch going out of range must not pause a podcast. It has no
        audio profile at all - battery service and device information only."""
        bus = BluezBus(["0000180f-0000-1000-8000-00805f9b34fb",
                        "0000180a-0000-1000-8000-00805f9b34fb"])
        self.assertFalse(watcher.carries_audio(bus, "/org/bluez/hci0/dev_BB"))

    def test_a_device_that_cannot_be_asked_counts_as_audio(self):
        """Doubt resolves towards pausing: an unpaired device is gone from the
        bus the moment it drops, and that one may well have been the earbuds."""
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertTrue(watcher.carries_audio(BluezBus(fail=True), "/x/dev_CC"))
        self.assertIn("treating it as audio", out.getvalue())

    def test_an_answer_that_makes_no_sense_counts_as_audio(self):
        for answer in ([], None, "not a list", 7):
            self.assertTrue(watcher.carries_audio(BluezBus(answer), "/x/dev_DD"),
                            "%r must not silence the safety net" % (answer,))

    def test_a_non_audio_device_leaves_playback_alone(self):
        """The whole handler, not just the predicate."""
        session = FakeBus(names=["org.mpris.MediaPlayer2.emilia"],
                          status={"org.mpris.MediaPlayer2.emilia": "Playing"})
        system = BluezBus(["0000180f-0000-1000-8000-00805f9b34fb"])
        buses = [system, session]

        original_get, original_loop = watcher.Gio.bus_get_sync, watcher.GLib.MainLoop
        original_timeout = watcher.GLib.timeout_add
        scheduled = []
        watcher.Gio.bus_get_sync = lambda kind, _c: buses.pop(0)
        watcher.GLib.MainLoop = lambda: type("Loop", (), {"run": lambda self: None})()
        watcher.GLib.timeout_add = lambda _ms, fn: scheduled.append(fn)
        try:
            out = io.StringIO()
            with redirect_stdout(out):
                watcher.main()
                handler = system.subscriptions[0][-1]
                handler(None, None, "/org/bluez/hci0/dev_BB", None, None,
                        FakeVariant(["org.bluez.Device1", {"Connected": False}, []]))
            self.assertEqual(session.paused, [])
            self.assertEqual(scheduled, [], "and no retries either")
            self.assertIn("carries no audio", out.getvalue())
        finally:
            watcher.Gio.bus_get_sync = original_get
            watcher.GLib.MainLoop = original_loop
            watcher.GLib.timeout_add = original_timeout

    def test_it_pauses_rather_than_muting_anything(self):
        """The alternative would be a phone that is silent for reasons nobody
        remembers, which is worse than one that was briefly too loud."""
        src = (ROOT / "tools" / "furios-audio-pause-on-disconnect.py").read_text()
        self.assertIn('"Pause"', src)
        self.assertNotIn("set-sink-mute", src)
        self.assertNotIn("set_mute", src)


class FakePactl:
    """Stands in for the two pactl calls hands_free_sink() makes."""

    def __init__(self, cards, sinks):
        self.cards, self.sinks = cards, sinks

    def run(self, argv, **_kwargs):
        which = "cards" if argv[-1] == "cards" else "sinks"
        return types.SimpleNamespace(stdout=self.cards if which == "cards" else self.sinks)


CARDS_HANDS_FREE = """Card #144
\tName: bluez_card.F4_9D_8A_7C_5C_66
\tActive Profile: headset-head-unit
"""
CARDS_A2DP = """Card #144
\tName: bluez_card.F4_9D_8A_7C_5C_66
\tActive Profile: a2dp-sink
"""
SINKS = "119\tdroid-sink\tPipeWire\n170\tbluez_output.F4_9D_8A_7C_5C_66.1\tPipeWire\n"


class ScoHoldFindsTheRightSink(unittest.TestCase):
    """Which sink may be held, and - far more important - which may not.

    In the A2DP profile bluez_output exists too, under the same name. A hold
    placed on THAT is not an SCO link at all: it holds music Bluetooth open for
    the length of a call, and the call stays silent while everything looks
    busy.
    """

    def use(self, cards, sinks=SINKS):
        sco.subprocess = FakePactl(cards, sinks)
        self.addCleanup(setattr, sco, "subprocess", subprocess_real)

    def test_the_hands_free_profile_gives_the_sink(self):
        self.use(CARDS_HANDS_FREE)
        self.assertEqual(sco.hands_free_sink(), "bluez_output.F4_9D_8A_7C_5C_66.1")

    def test_a2dp_gives_nothing(self):
        self.use(CARDS_A2DP)
        self.assertIsNone(sco.hands_free_sink())

    def test_the_narrow_band_profile_counts_too(self):
        self.use(CARDS_HANDS_FREE.replace("headset-head-unit", "headset-head-unit-cvsd"))
        self.assertEqual(sco.hands_free_sink(), "bluez_output.F4_9D_8A_7C_5C_66.1")

    def test_no_bluetooth_card_gives_nothing(self):
        self.use("Card #118\n\tName: droid\n\tActive Profile: default\n")
        self.assertIsNone(sco.hands_free_sink())

    def test_a_profile_but_no_bluetooth_sink_gives_nothing(self):
        self.use(CARDS_HANDS_FREE, sinks="119\tdroid-sink\tPipeWire\n")
        self.assertIsNone(sco.hands_free_sink())

    def test_the_droid_cards_profile_is_not_mistaken_for_the_headsets(self):
        # The phone card is listed first and has an Active Profile of its own.
        # Reading the wrong one would hold a link in the A2DP profile, which is
        # the failure this whole function exists to avoid.
        self.use("Card #118\n\tName: droid\n\tActive Profile: headset-head-unit\n"
                 "Card #144\n\tName: bluez_card.X\n\tActive Profile: a2dp-sink\n")
        self.assertIsNone(sco.hands_free_sink())


class FakePopen:
    """A process that is alive until somebody says otherwise."""

    started = []

    def __init__(self, argv, **_kwargs):
        self.argv, self.pid, self._rc = argv, 4242, None
        FakePopen.started.append(argv)

    def poll(self):
        return self._rc

    def terminate(self):
        self._rc = -15


class ScoHoldBehaviour(unittest.TestCase):
    """When it holds, when it refuses, and that it always lets go."""

    def setUp(self):
        FakePopen.started = []
        self.fake = types.SimpleNamespace(Popen=FakePopen, DEVNULL=-3,
                                          SubprocessError=Exception)
        sco.subprocess = self.fake
        self.addCleanup(setattr, sco, "subprocess", subprocess_real)

    def hold_with(self, sink):
        hold = sco.Hold()
        sco.hands_free_sink = lambda: sink
        self.addCleanup(setattr, sco, "hands_free_sink", hands_free_sink_real)
        with redirect_stdout(io.StringIO()):
            hold.begin()
        return hold

    def test_it_holds_when_the_profile_is_there(self):
        self.hold_with("bluez_output.X")
        self.assertEqual(len(FakePopen.started), 1)
        argv = FakePopen.started[0]
        self.assertEqual(argv[0], "paplay")
        self.assertIn("--device=bluez_output.X", argv)
        # 16 kHz mono silence: the rate SCO runs at. Anything else is resampled
        # into a link that has no room for it.
        self.assertIn("--rate=16000", argv)
        self.assertIn("--channels=1", argv)
        self.assertEqual(argv[-1], "/dev/zero")

    def test_it_holds_nothing_without_a_hands_free_profile(self):
        self.hold_with(None)
        self.assertEqual(FakePopen.started, [])

    def test_it_gives_up_rather_than_wait_forever(self):
        hold = sco.Hold()
        sco.hands_free_sink = lambda: None
        self.addCleanup(setattr, sco, "hands_free_sink", hands_free_sink_real)
        out = io.StringIO()
        with redirect_stdout(out):
            hold.begin(waited=sco.MAX_WAIT_MS)
        self.assertEqual(FakePopen.started, [])
        # And it says why, naming the setting - a call that is silently not
        # held looks exactly like a headset that is broken.
        self.assertIn("furios.bluetooth-call-routing", out.getvalue())

    def test_stopping_ends_the_stream(self):
        hold = self.hold_with("bluez_output.X")
        proc = hold.proc
        with redirect_stdout(io.StringIO()):
            hold.stop()
        self.assertIsNone(hold.proc)
        self.assertIsNotNone(proc.poll())

    def test_stopping_twice_is_harmless(self):
        hold = self.hold_with("bluez_output.X")
        with redirect_stdout(io.StringIO()):
            hold.stop()
            hold.stop()

    def test_a_second_call_does_not_start_a_second_stream(self):
        hold = self.hold_with("bluez_output.X")
        with redirect_stdout(io.StringIO()):
            hold.begin()
        self.assertEqual(len(FakePopen.started), 1)

    def test_a_stream_that_keeps_dying_is_not_restarted_forever(self):
        hold = self.hold_with("bluez_output.X")
        with redirect_stdout(io.StringIO()):
            for _ in range(sco.MAX_RESTARTS + 3):
                hold._died(4242, 1)
        # The first stream plus exactly MAX_RESTARTS replacements, and then it
        # stops - not "at most", or a version that never restarts at all would
        # pass this too.
        self.assertEqual(len(FakePopen.started), sco.MAX_RESTARTS + 1)

    def test_a_deliberate_stop_is_not_treated_as_a_death(self):
        # stop() clears proc first; the child watch then fires. Restarting
        # there would put the stream back up seconds after the call ended and
        # leave the earbuds in hands-free.
        hold = self.hold_with("bluez_output.X")
        with redirect_stdout(io.StringIO()):
            hold.stop()
            hold._died(4242, 0)
        self.assertEqual(len(FakePopen.started), 1)


class OfonoBus:
    """A system bus that answers ofono's two questions and records subscriptions."""

    def __init__(self, modems=(), calls=None, fail=None):
        self.modems = list(modems)
        self.calls = calls or {}
        self.fail = fail or set()
        self.subscriptions = []

    def call_sync(self, dest, path, iface, method, args, reply, flags,
                  timeout, cancellable):
        if method in self.fail:
            raise sco.GLib.Error("ofono is not answering")
        if method == "GetModems":
            return FakeVariant([[(m, {}) for m in self.modems]])
        if method == "GetCalls":
            if path in self.fail:
                raise sco.GLib.Error("modem went away")
            return FakeVariant([[(c, {}) for c in self.calls.get(path, [])]])
        raise AssertionError("unexpected call: %s" % method)

    def signal_subscribe(self, *args):
        self.subscriptions.append(args)
        return 1


class ScoHoldTalksToOfono(unittest.TestCase):
    """Which calls it thinks are up - including the ones already up when it starts."""

    def test_calls_already_up_are_found(self):
        bus = OfonoBus(modems=["/ril_0"], calls={"/ril_0": ["/ril_0/voicecall01"]})
        self.assertEqual(sco.existing_calls(bus), {"/ril_0/voicecall01"})

    def test_no_calls_is_an_empty_set_not_an_error(self):
        self.assertEqual(sco.existing_calls(OfonoBus(modems=["/ril_0"])), set())

    def test_an_ofono_that_does_not_answer_is_survived(self):
        out = io.StringIO()
        with redirect_stdout(out):
            found = sco.existing_calls(OfonoBus(fail={"GetModems"}))
        self.assertEqual(found, set())
        self.assertIn("could not ask ofono", out.getvalue())

    def test_one_modem_failing_does_not_lose_the_others(self):
        bus = OfonoBus(modems=["/ril_0", "/ril_1"],
                       calls={"/ril_1": ["/ril_1/voicecall01"]},
                       fail={"/ril_0"})
        self.assertEqual(sco.existing_calls(bus), {"/ril_1/voicecall01"})


class ScoHoldMain(unittest.TestCase):
    """main() only wires things up; the handlers are what decide."""

    def setUp(self):
        FakePopen.started = []
        sco.subprocess = types.SimpleNamespace(Popen=FakePopen, DEVNULL=-3,
                                               SubprocessError=Exception)
        self.addCleanup(setattr, sco, "subprocess", subprocess_real)
        sco.hands_free_sink = lambda: "bluez_output.X"
        self.addCleanup(setattr, sco, "hands_free_sink", hands_free_sink_real)
        self.bus = OfonoBus()
        self.ran = []
        originals = (sco.Gio.bus_get_sync, sco.GLib.MainLoop, sco.shutil.which)
        self.addCleanup(self.restore, originals)
        sco.Gio.bus_get_sync = lambda _kind, _c: self.bus
        sco.GLib.MainLoop = lambda: type(
            "Loop", (), {"run": lambda _self: self.ran.append(True),
                         "quit": lambda _self: None})()
        sco.shutil.which = lambda _tool: "/usr/bin/" + _tool

    def restore(self, originals):
        sco.Gio.bus_get_sync, sco.GLib.MainLoop, sco.shutil.which = originals

    def run_main(self):
        out = io.StringIO()
        with redirect_stdout(out):
            sco.main()
        return out.getvalue()

    def handlers(self):
        # CallAdded was subscribed first, CallRemoved second.
        return self.bus.subscriptions[0][-1], self.bus.subscriptions[1][-1]

    def test_it_subscribes_to_both_ends_of_a_call(self):
        text = self.run_main()
        self.assertIn("watching ofono", text)
        self.assertTrue(self.ran, "it has to keep running, not return at once")
        members = [args[2] for args in self.bus.subscriptions]
        self.assertEqual(members, ["CallAdded", "CallRemoved"])

    def test_a_call_starts_a_hold_and_its_end_stops_it(self):
        self.run_main()
        added, removed = self.handlers()
        with redirect_stdout(io.StringIO()):
            added(None, None, None, None, None, FakeVariant(["/ril_0/voicecall01"]))
            self.assertEqual(len(FakePopen.started), 1)
            removed(None, None, None, None, None, FakeVariant(["/ril_0/voicecall01"]))
        self.assertEqual(FakePopen.started[0][0], "paplay")

    def test_a_second_call_does_not_start_a_second_hold(self):
        # Call waiting: a second call arrives while the first is up. One link
        # is one link; starting another stream would not make a second one.
        self.run_main()
        added, _removed = self.handlers()
        with redirect_stdout(io.StringIO()):
            added(None, None, None, None, None, FakeVariant(["/ril_0/voicecall01"]))
            added(None, None, None, None, None, FakeVariant(["/ril_0/voicecall02"]))
        self.assertEqual(len(FakePopen.started), 1)

    def test_the_hold_survives_one_of_two_calls_ending(self):
        self.run_main()
        added, removed = self.handlers()
        with redirect_stdout(io.StringIO()):
            added(None, None, None, None, None, FakeVariant(["/ril_0/voicecall01"]))
            added(None, None, None, None, None, FakeVariant(["/ril_0/voicecall02"]))
            hung_up = FakeVariant(["/ril_0/voicecall02"])
            removed(None, None, None, None, None, hung_up)
        # One call is still up, so the link must still be held - the other
        # party is still on it.
        self.assertEqual(len(FakePopen.started), 1)

    def test_starting_during_a_call_holds_straight_away(self):
        self.bus = OfonoBus(modems=["/ril_0"], calls={"/ril_0": ["/ril_0/voicecall01"]})
        text = self.run_main()
        self.assertIn("started during a call", text)
        self.assertEqual(len(FakePopen.started), 1)

    def test_without_paplay_it_says_so_and_stops(self):
        sco.shutil.which = lambda tool: None if tool == "paplay" else "/usr/bin/pactl"
        text = self.run_main()
        self.assertIn("paplay is not installed", text)
        self.assertFalse(self.ran, "there is nothing to watch for")


class ScoHoldSurvivesBadDays(unittest.TestCase):
    """The failures that must not take the phone's audio with them."""

    def test_a_pactl_that_does_not_answer_holds_nothing(self):
        class Broken:
            SubprocessError = Exception

            def run(self, *_a, **_k):
                raise OSError("pactl is not there")

        sco.subprocess = Broken()
        self.addCleanup(setattr, sco, "subprocess", subprocess_real)
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertIsNone(sco.hands_free_sink())
        self.assertIn("could not ask pactl", out.getvalue())

    def test_a_pactl_that_answers_about_cards_but_not_sinks(self):
        class HalfBroken:
            SubprocessError = Exception

            def run(self, argv, **_k):
                if argv[-1] == "cards":
                    return types.SimpleNamespace(stdout=CARDS_HANDS_FREE)
                raise OSError("gone between the two questions")

        sco.subprocess = HalfBroken()
        self.addCleanup(setattr, sco, "subprocess", subprocess_real)
        with redirect_stdout(io.StringIO()):
            self.assertIsNone(sco.hands_free_sink())

    def test_a_paplay_that_will_not_start_is_not_a_crash(self):
        class NoPopen:
            DEVNULL = -3
            SubprocessError = Exception

            def Popen(self, *_a, **_k):
                raise OSError("no such binary")

        sco.subprocess = NoPopen()
        self.addCleanup(setattr, sco, "subprocess", subprocess_real)
        hold = sco.Hold()
        out = io.StringIO()
        with redirect_stdout(out):
            hold.start_on("bluez_output.X")
        self.assertIsNone(hold.proc)
        self.assertIn("could not start the hold", out.getvalue())

    def test_a_call_that_never_ends_does_not_hold_the_link_all_day(self):
        # A missed CallRemoved would otherwise leave the earbuds in hands-free
        # for the rest of the day: music narrow-band and mono, and nothing on
        # screen to say why.
        FakePopen.started = []
        sco.subprocess = types.SimpleNamespace(Popen=FakePopen, DEVNULL=-3,
                                               SubprocessError=Exception)
        self.addCleanup(setattr, sco, "subprocess", subprocess_real)
        hold = sco.Hold()
        with redirect_stdout(io.StringIO()):
            hold.start_on("bluez_output.X")
            proc = hold.proc
            hold._too_long()
        self.assertIsNone(hold.proc)
        self.assertIsNotNone(proc.poll())


class ScoHoldLetsGoOnTheWayOut(unittest.TestCase):
    """Being stopped has to release the link, or the earbuds stay in hands-free.

    systemctl stop, a session ending, a reboot: whatever the reason, a hold
    that outlives the service leaves music narrow-band and mono with nothing on
    screen to say why.
    """

    def test_a_termination_signal_stops_the_hold(self):
        FakePopen.started = []
        sco.subprocess = types.SimpleNamespace(Popen=FakePopen, DEVNULL=-3,
                                               SubprocessError=Exception)
        sco.hands_free_sink = lambda: "bluez_output.X"
        bus = OfonoBus(modems=["/ril_0"], calls={"/ril_0": ["/ril_0/voicecall01"]})
        quits, handlers = [], []
        originals = (sco.Gio.bus_get_sync, sco.GLib.MainLoop, sco.shutil.which,
                     sco.unix_signal_add)
        self.addCleanup(setattr, sco, "subprocess", subprocess_real)
        self.addCleanup(setattr, sco, "hands_free_sink", hands_free_sink_real)

        def restore():
            (sco.Gio.bus_get_sync, sco.GLib.MainLoop, sco.shutil.which,
             sco.unix_signal_add) = originals
        self.addCleanup(restore)

        sco.Gio.bus_get_sync = lambda _kind, _c: bus
        sco.GLib.MainLoop = lambda: type(
            "Loop", (), {"run": lambda _s: None,
                         "quit": lambda _s: quits.append(True)})()
        sco.shutil.which = lambda tool: "/usr/bin/" + tool
        sco.unix_signal_add = lambda _prio, _sig, fn: handlers.append(fn)

        with redirect_stdout(io.StringIO()):
            sco.main()
            # SIGTERM and SIGINT both, and both have to let go.
            self.assertEqual(len(handlers), 2)
            proc = None
            for line in FakePopen.started:
                self.assertEqual(line[0], "paplay")
            handlers[0]()
        self.assertTrue(quits, "the loop has to be asked to stop")

    def test_without_the_new_signal_module_the_old_spelling_is_used(self):
        # Both spellings are in the wild; the phone has the new one. Load the
        # module again with it hidden and check the fallback is real.
        import gi as gi_mod
        saved = sys.modules.pop("gi.repository.GLibUnix", None)
        removed = gi_mod.repository.GLibUnix
        del gi_mod.repository.GLibUnix
        try:
            again = load(ROOT / "tools" / "furios-audio-sco-hold.py", "sco_again")
            self.assertIs(again.unix_signal_add, again.GLib.unix_signal_add)
        finally:
            gi_mod.repository.GLibUnix = removed
            if saved is not None:
                sys.modules["gi.repository.GLibUnix"] = saved


if __name__ == "__main__":
    # Built by hand rather than through unittest.main(), which looks for tests
    # in sys.modules["__main__"] - and under the coverage tracer that is the
    # tracer, not this file. It finds nothing there and says so quietly.
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()
    for obj in list(globals().values()):
        if isinstance(obj, type) and issubclass(obj, unittest.TestCase):
            suite.addTests(loader.loadTestsFromTestCase(obj))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)
