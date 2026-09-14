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
import shutil as shutil_real
import subprocess as subprocess_real
import tempfile
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
switcher = load(ROOT / "gui" / "misc-de.py", "switcher")
sco = load(ROOT / "tools" / "furios-audio-sco-hold.py", "sco_hold")
btmic = load(ROOT / "tools" / "furios-audio-bt-mic.py", "bt_mic")
hands_free_sink_real = sco.hands_free_sink
os_real = sco.os


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
        cls.app = (ROOT / "gui" / "misc-de.py").read_text()

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


    def test_a_tool_that_is_nowhere_is_admitted_rather_than_invented(self):
        """_tool_maybe decides whether the modem page exists at all. Guessing a
        path here would build a whole tab around a program that is not
        installed, and every row in it would report a failure of its own."""
        original_access, original_which = switcher.os.access, switcher.shutil.which
        switcher.os.access = lambda path, mode: False
        switcher.shutil.which = lambda name: None
        try:
            self.assertIsNone(switcher._tool_maybe("modemctl"))
        finally:
            switcher.os.access, switcher.shutil.which = original_access, original_which

    def test_a_tool_only_on_the_search_path_is_still_found(self):
        original_access, original_which = switcher.os.access, switcher.shutil.which
        switcher.os.access = lambda path, mode: False
        switcher.shutil.which = lambda name: "/opt/bin/" + name
        try:
            self.assertEqual("/opt/bin/modemctl", switcher._tool_maybe("modemctl"))
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
        names = ["row_profile", "row_server", "row_sinks", "switch_row",
                 "persist_row", "dmnr_row", "refresh_btn", "progress",
                 "progress_revealer", "toasts"]
        # The modem widgets only exist when the page was built, and the page is
        # only built when modemctl is installed - so they are swapped in the
        # same way, and only when they are there to swap.
        if switcher.MODEMCTL:
            names += ["modem_row", "modem_persist", "modem_progress",
                      "modem_revealer", "mrow_profile", "mrow_health",
                      "mrow_signal", "modem_restore_btn"]
        for name in names:
            setattr(self.win, name, Recording())
        if switcher.MODEMCTL:
            self.win.modem_rows = [self.win.modem_row, self.win.modem_persist,
                                   self.win.modem_restore_btn]
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

    PERMANENT = ("Profile (active):   pw-hal\n"
                 "Profile (persistent): pw-hal\n"
                 "Pulse server:       PulseAudio (on PipeWire 1.6.6)\n"
                 "Sinks:              droid-sink\n")

    def test_status_is_turned_into_something_a_person_can_read(self):
        self.win.on_status(True, self.STATUS)
        self.assertIn("PipeWire owns the HAL", self.win.row_profile.subtitle)
        self.assertIn("until the next reboot", self.win.row_profile.subtitle)
        self.assertIn("older apps", self.win.row_server.subtitle)
        self.assertEqual("droid-sink, droid-voip-sink", self.win.row_sinks.subtitle)
        self.assertTrue(self.win.switch_row.active)

    def test_a_profile_that_survives_a_reboot_says_so(self):
        """The state the phone had for two days while this window said nothing.

        "Profile (persistent)" was never read, so the remembered-switch sat at
        its default - off - on a phone that was on pw-hal for good.
        """
        self.win.on_status(True, self.PERMANENT)
        self.assertIn("permanent", self.win.row_profile.subtitle)
        self.assertTrue(self.win.persist_row.active,
                        "a permanent profile was shown as not remembered")
        self.assertIn("comes back to", self.win.persist_row.subtitle)

    def test_a_profile_that_only_holds_until_the_reboot_says_what_returns(self):
        self.win.on_status(True, self.STATUS)
        self.assertFalse(self.win.persist_row.active)
        self.assertIn("PulseAudio", self.win.persist_row.subtitle)

    def test_a_status_without_the_persistent_line_claims_nothing(self):
        """An audioctl too old to print it must not produce an invented state."""
        self.win.on_status(True, "Profile (active):   pw-hal\nSinks:              x\n")
        self.assertNotIn("permanent", self.win.row_profile.subtitle)
        self.assertNotIn("until the next reboot", self.win.row_profile.subtitle)
        self.assertFalse(self.win.persist_row.active)

    def test_audioctl_not_answering_claims_nothing_about_the_phone(self):
        """Off is the shipped state, so a switch left at off is not a blank -
        it is a plausible statement about a phone this window cannot see."""
        self.win.switch_row.active = True
        self.win.on_status(False, "Failed to execute child process")
        self.assertIn("did not answer", self.win.row_profile.subtitle)
        self.assertIn("did not answer", self.win.persist_row.subtitle)
        self.assertTrue(self.win.switch_row.active,
                        "the switch was moved on the strength of no answer")
        self.assertFalse(self.win.switch_row.sensitive)
        self.assertFalse(self.win.persist_row.sensitive)

    def test_a_control_with_nothing_behind_it_stays_unusable(self):
        """set_busy used to be the only hand on the sensitivity, so the next
        finished action handed back a switch that leads nowhere."""
        self.win.on_status(False, "")
        self.win.set_busy(True)
        self.win.set_busy(False)
        self.assertFalse(self.win.switch_row.sensitive)
        self.assertFalse(self.win.persist_row.sensitive)
        self.assertIn("did not answer", self.win.switch_row.subtitle)

    def test_and_becomes_usable_again_once_audioctl_answers(self):
        self.win.on_status(False, "")
        self.win.on_status(True, self.PERMANENT)
        self.assertTrue(self.win.switch_row.sensitive)
        self.assertIn("PipeWire", self.win.switch_row.subtitle)

    def test_an_echo_switch_with_no_script_behind_it_stays_off_limits(self):
        self.win.on_dmnr_status(False, "")
        self.win.set_busy(True)
        self.win.set_busy(False)
        self.assertFalse(self.win.dmnr_row.sensitive)
        self.assertIn("not available", self.win.dmnr_row.subtitle)

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

    def test_refresh_asks_every_helper_there_is(self):
        """Two for audio, and two more for the modem when modemctl is here.

        Counted rather than named, because the point is that adding a page
        must not leave one of the others unasked - which is how a window ends
        up showing a state that stopped being true ten minutes ago."""
        self.win.refresh()
        expected = 2 + (2 if switcher.MODEMCTL else 0)
        self.assertEqual(expected, len(self.ran))

    # --- the modem page ----------------------------------------------------
    #
    # It is built only when modemctl is installed, so every check here says so
    # first. On a phone without the modem package there is no second tab and
    # nothing below has anything to test - which is itself the behaviour that
    # matters most: a tab that always says "not installed" would make a healthy
    # phone look broken.

    def test_without_modemctl_there_is_no_second_page(self):
        real = switcher.MODEMCTL
        try:
            switcher.MODEMCTL = None
            win = switcher.Window(switcher.Adw.Application())
            self.assertEqual([], win.modem_rows)
        finally:
            switcher.MODEMCTL = real

    def test_the_two_words_the_profile_is_read_by(self):
        """modemctl lives in another package. These two keys are the contract
        between them, and its other half is checked over there, where they are
        printed."""
        found = switcher.Window._keyed("recorded: fixed\nactual:   shipped\n")
        self.assertEqual({"recorded": "fixed", "actual": "shipped"}, found)

    def modem_win(self):
        if not switcher.MODEMCTL:
            self.skipTest("no modemctl on this machine, so no modem page")
        return self.win

    def test_a_repaired_modem_reads_as_repaired(self):
        win = self.modem_win()
        win.on_modem_profile(True, "recorded: fixed\nactual:   fixed\n")
        self.assertTrue(win.modem_row.get_active())
        self.assertIn("repairs are in place", win.mrow_profile.subtitle)

    def test_a_try_says_the_next_boot_will_undo_it(self):
        win = self.modem_win()
        win.on_modem_profile(True, "recorded: fixed\nactual:   shipped\n")
        self.assertFalse(win.modem_row.get_active())
        self.assertIn("next boot", win.mrow_profile.subtitle)

    def test_half_repaired_is_not_dressed_up_as_either(self):
        win = self.modem_win()
        win.on_modem_profile(True, "recorded: fixed\nactual:   mixed\n")
        self.assertIn("half repaired", win.mrow_profile.subtitle)

    def test_the_switch_asks_for_the_rights_it_needs(self):
        win = self.modem_win()
        win.modem_persist.active = True
        win.modem_row.active = False
        win.on_modem_switch(win.modem_row, None)
        argv = self.ran[-1][0]
        self.assertIn("pkexec", argv[0])
        self.assertEqual(["set", "shipped"], argv[2:])

    def test_modemctl_not_answering_is_said_and_not_guessed(self):
        """A profile row that invents "shipped" when it was told nothing would
        make a repaired phone look untouched."""
        win = self.modem_win()
        win.on_modem_profile(False, "")
        self.assertIn("did not answer", win.mrow_profile.subtitle)
        self.assertFalse(win.modem_row.sensitive,
                         "the switch stayed usable with nothing behind it")

    def test_a_modem_page_with_no_modemctl_answer_stays_unusable(self):
        win = self.modem_win()
        win.on_modem_profile(False, "")
        win.set_busy(True)
        win.set_busy(False)
        self.assertFalse(win.modem_row.sensitive)
        self.assertFalse(win.modem_restore_btn.sensitive,
                         "the restore button came back with nothing behind it")

    def test_the_checks_are_counted_the_way_modemctl_prints_them(self):
        win = self.modem_win()
        win.on_modem_status(True,
                            "  ok    utils.py\n"
                            "  ok    mobile default route: ccmni0\n"
                            "  FAIL  resolv.conf -> systemd-resolved\n"
                            "  ok    signal quality 26% (recent)\n")
        self.assertIn("3 in place, 1 not", win.mrow_health.subtitle)
        self.assertIn("26%", win.mrow_signal.subtitle)

    def test_everything_in_place_is_not_dressed_up_with_a_zero(self):
        win = self.modem_win()
        win.on_modem_status(True, "  ok    utils.py\n  ok    main.py\n")
        self.assertEqual("2 in place", win.mrow_health.subtitle)

    def test_a_status_without_a_signal_line_says_so(self):
        win = self.modem_win()
        win.on_modem_status(True, "  ok    utils.py\n")
        self.assertIn("not readable", win.mrow_signal.subtitle)

    def test_a_status_that_failed_but_printed_is_still_read(self):
        """modemctl exits non-zero when a check fails - and then its output is
        exactly the thing worth showing."""
        win = self.modem_win()
        win.on_modem_status(False, "  ok    utils.py\n  FAIL  mobile default route\n")
        self.assertIn("1 in place, 1 not", win.mrow_health.subtitle)

    def test_a_status_with_no_output_at_all_claims_nothing(self):
        win = self.modem_win()
        win.on_modem_status(False, "")
        self.assertIn("did not answer", win.mrow_health.subtitle)

    def test_without_pkexec_the_switch_says_so_and_runs_nothing(self):
        win = self.modem_win()
        real = switcher.PKEXEC
        try:
            switcher.PKEXEC = None
            win.modem_row.active = False
            win.on_modem_switch(win.modem_row, None)
            self.assertEqual([], self.ran, "it tried to switch without rights")
            self.assertIn("pkexec", str(win.toasts.text))
        finally:
            switcher.PKEXEC = real

    def test_without_pkexec_the_restore_button_says_so_and_runs_nothing(self):
        win = self.modem_win()
        real = switcher.PKEXEC
        try:
            switcher.PKEXEC = None
            win.on_modem_restore(None)
            self.assertEqual([], self.ran, "it tried to restore without rights")
            self.assertIn("pkexec", str(win.toasts.text))
        finally:
            switcher.PKEXEC = real

    def test_a_finished_switch_repeats_modemctls_last_word(self):
        win = self.modem_win()
        win.on_modem_switched(True, "Recorded: shipped. This survives a reboot.\n\n")
        self.assertIn("Recorded: shipped", str(win.toasts.text))

    def test_a_switch_that_printed_nothing_still_says_something(self):
        win = self.modem_win()
        win.on_modem_switched(True, "")
        self.assertIn("Done", str(win.toasts.text))

    def test_a_refused_switch_is_reported_as_a_failure(self):
        """polkit saying no looks like any other failure from here, and on a
        phone where this is not authorised it is the likely one."""
        win = self.modem_win()
        win.on_modem_switched(False, "Error executing command as another user")
        self.assertIn("failed", str(win.toasts.text).lower())

    def test_the_restore_button_asks_for_the_shipped_state_for_good(self):
        """The counterpart to "Restore sound": what it restores has to be what
        the phone comes back to, so "set" and never "try"."""
        win = self.modem_win()
        win.on_modem_restore(None)
        argv = self.ran[-1][0]
        self.assertIn("pkexec", argv[0])
        self.assertEqual(["set", "shipped"], argv[2:])

    def test_the_restore_button_ignores_the_remember_switch(self):
        win = self.modem_win()
        win.modem_persist.active = False
        win.on_modem_restore(None)
        self.assertEqual(["set", "shipped"], self.ran[-1][0][2:])

    def test_restoring_says_what_the_phone_is_now(self):
        win = self.modem_win()
        win.on_modem_restored(True, "")
        self.assertIn("no network without Wi-Fi", str(win.toasts.text))

    def test_a_failed_restore_is_not_reported_as_done(self):
        win = self.modem_win()
        win.on_modem_restored(False, "revert failed")
        self.assertIn("Could not restore", str(win.toasts.text))

    def test_the_restore_button_does_nothing_while_busy(self):
        win = self.modem_win()
        win.busy = True
        win.on_modem_restore(None)
        self.assertEqual([], self.ran)

    def test_not_remembering_is_a_try_and_not_a_set(self):
        win = self.modem_win()
        win.modem_persist.active = False
        win.modem_row.active = True
        win.on_modem_switch(win.modem_row, None)
        self.assertEqual(["try", "fixed"], self.ran[-1][0][2:])

    def test_the_switch_does_not_fire_while_the_window_is_syncing(self):
        """Filling the switch from the status would otherwise switch the modem."""
        win = self.modem_win()
        win._syncing = True
        win.on_modem_switch(win.modem_row, None)
        win._syncing = False
        self.assertEqual([], self.ran)

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

    SubprocessError = Exception

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


class ScoHoldSaysWhyItHeldNothing(unittest.TestCase):
    """Two ways to hold nothing, and only one of them is a fault.

    Every call on the earpiece ends up here, so a warning that names the
    routing setting would be printed for calls that never had a headset in
    them. A journal full of those is how the next search into a silent
    Bluetooth call starts by ruling out the wrong thing.
    """

    def gave_up_with(self, cards):
        sco.subprocess = FakePactl(cards, SINKS)
        self.addCleanup(setattr, sco, "subprocess", subprocess_real)
        out = io.StringIO()
        with redirect_stdout(out):
            sco.Hold().begin(waited=sco.MAX_WAIT_MS)
        return out.getvalue()

    def test_no_headset_at_all_is_not_worth_a_warning(self):
        text = self.gave_up_with("Card #118\n\tName: droid\n\tActive Profile: default\n")
        self.assertIn("no Bluetooth headset", text)
        self.assertNotIn("furios.bluetooth-call-routing", text)

    def test_a_headset_that_never_switched_is_worth_one(self):
        # The headset is right there in A2DP and the call is up: the routing
        # was supposed to move it and did not. That is the case the setting is
        # named for.
        text = self.gave_up_with(CARDS_A2DP)
        self.assertIn("furios.bluetooth-call-routing", text)


class FakeOs:
    """os, but it cannot reach anything that is really running.

    stop() kills the process group the stream was started in. FakePopen makes
    its pid up, and on a machine where that number happens to belong to a real
    process - on a freshly booted phone 4242 is an ordinary pid - the real
    os.getpgid succeeds and the suite sends SIGTERM to whatever that is. It
    also made the test pass or fail depending on what else was running, which
    is how it was found.
    """

    def __init__(self, gone=False):
        self.killed = []
        self.gone = gone

    def getpgid(self, pid):
        if self.gone:
            raise ProcessLookupError("no such process")
        return pid

    def killpg(self, pgid, sig):
        self.killed.append((pgid, sig))


class FakePopen:
    """A process that is alive until somebody says otherwise."""

    started = []

    PID = 4242

    def __init__(self, argv, **_kwargs):
        self.argv, self.pid, self._rc = argv, FakePopen.PID, None
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
        self.os = FakeOs()
        sco.os = self.os
        self.addCleanup(setattr, sco, "os", os_real)

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
        # And it says why - a call that is silently not held looks exactly
        # like a headset that is broken. Which of the two reasons it gives is
        # ScoHoldSaysWhyItHeldNothing's business.
        self.assertIn("nothing to hold", out.getvalue())

    def test_stopping_ends_the_stream(self):
        hold = self.hold_with("bluez_output.X")
        pid = hold.proc.pid
        with redirect_stdout(io.StringIO()):
            hold.stop()
        self.assertIsNone(hold.proc)
        # The group, not the process: the stream runs in a session of its own
        # so that a profile change cannot take it down, and only the group
        # kill reaches it there.
        self.assertEqual(self.os.killed, [(pid, sco.signal.SIGTERM)])

    def test_a_stream_whose_group_is_already_gone_is_not_an_error(self):
        hold = self.hold_with("bluez_output.X")
        sco.os = FakeOs(gone=True)
        proc = hold.proc
        with redirect_stdout(io.StringIO()):
            hold.stop()
        self.assertIsNotNone(proc.poll(), "it has to fall back to terminate()")

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

    def test_a_poll_left_over_from_an_ended_call_holds_nothing(self):
        """The bug this guards: a call short enough to end while the wait for
        the hands-free profile is still running - one ring and a rejection -
        left the poll in flight. It fired afterwards, found the headset still
        in hands-free because the routing had not switched back yet, and put a
        silent stream on it with no call to end it. Nothing would have taken it
        off again until the three-hour deadline."""
        hold = sco.Hold()
        sco.hands_free_sink = lambda: "bluez_output.X"
        self.addCleanup(setattr, sco, "hands_free_sink", hands_free_sink_real)
        epoch = hold.epoch
        with redirect_stdout(io.StringIO()):
            hold.stop()                       # the call ended first
            hold.begin(waited=sco.POLL_MS, epoch=epoch)
        self.assertEqual(FakePopen.started, [],
                         "a poll from a call that is over must hold nothing")

    def test_the_wait_for_a_new_call_is_not_called_off_by_the_old_one(self):
        # The epoch must not be so blunt that it kills the call after it too.
        hold = sco.Hold()
        sco.hands_free_sink = lambda: "bluez_output.X"
        self.addCleanup(setattr, sco, "hands_free_sink", hands_free_sink_real)
        with redirect_stdout(io.StringIO()):
            hold.stop()
            hold.begin(waited=sco.POLL_MS, epoch=hold.epoch)
        self.assertEqual(len(FakePopen.started), 1)

    def test_stopping_leaves_the_child_watch_to_reap_the_stream(self):
        # Taking the watch away and only then killing the process leaves
        # nobody to reap it: one zombie per call. _died already ignores a
        # deliberate stop, so the watch can simply be left alone.
        hold = self.hold_with("bluez_output.X")
        with redirect_stdout(io.StringIO()):
            hold.stop()
        self.assertIsNotNone(hold.watch,
                             "the watch has to outlive the kill to reap it")

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
        self.os = FakeOs()
        sco.os = self.os
        self.addCleanup(setattr, sco, "os", os_real)
        sco.hands_free_sink = lambda: "bluez_output.X"
        self.addCleanup(setattr, sco, "hands_free_sink", hands_free_sink_real)
        self.bus = OfonoBus()
        self.ran = []
        self.watchers = []
        originals = (sco.Gio.bus_get_sync, sco.GLib.MainLoop, sco.shutil.which,
                     sco.Gio.bus_watch_name_on_connection)
        self.addCleanup(self.restore, originals)
        sco.Gio.bus_get_sync = lambda _kind, _c: self.bus
        sco.Gio.bus_watch_name_on_connection = self.watch_name
        sco.GLib.MainLoop = lambda: type(
            "Loop", (), {"run": lambda _self: self.ran.append(True),
                         "quit": lambda _self: None})()
        sco.shutil.which = lambda _tool: "/usr/bin/" + _tool

    def restore(self, originals):
        (sco.Gio.bus_get_sync, sco.GLib.MainLoop, sco.shutil.which,
         sco.Gio.bus_watch_name_on_connection) = originals

    def watch_name(self, _conn, _name, _flags, appeared, vanished):
        self.watchers.append((appeared, vanished))
        return 1

    def ofono_appears(self):
        """ofono claiming the name - at boot it does so after this service."""
        out = io.StringIO()
        with redirect_stdout(out):
            for appeared, _vanished in self.watchers:
                appeared(None, "org.ofono", ":1.5")
        return out.getvalue()

    def ofono_vanishes(self):
        out = io.StringIO()
        with redirect_stdout(out):
            for _appeared, vanished in self.watchers:
                vanished(None, "org.ofono")
        return out.getvalue()

    def run_main(self):
        out = io.StringIO()
        with redirect_stdout(out):
            sco.main()
        return out.getvalue()

    def handlers(self):
        # CallAdded was subscribed first, CallRemoved second.
        return self.bus.subscriptions[0][-1], self.bus.subscriptions[1][-1]

    def test_it_subscribes_to_both_ends_of_a_call(self):
        self.run_main()
        self.assertTrue(self.ran, "it has to keep running, not return at once")
        members = [args[2] for args in self.bus.subscriptions]
        self.assertEqual(members, ["CallAdded", "CallRemoved"])
        self.assertIn("watching ofono", self.ofono_appears())

    def test_it_subscribes_before_asking_what_is_up(self):
        # In the other order a call that starts between the asking and the
        # subscribing is never seen at all - and at boot the asking is the
        # part that has to wait for ofono.
        self.bus = OfonoBus(modems=["/ril_0"])
        self.run_main()
        self.assertTrue(self.bus.subscriptions,
                        "subscribed too late to be sure of catching a call")

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
        self.run_main()
        self.assertIn("a call is already up", self.ofono_appears())
        self.assertEqual(len(FakePopen.started), 1)

    def test_asking_ofono_waits_until_ofono_is_there(self):
        # At boot this service starts before ofono, and asking straight away
        # only put a ServiceUnknown in the journal. Nothing may be asked until
        # the name shows up.
        self.bus = OfonoBus(modems=["/ril_0"], calls={"/ril_0": ["/ril_0/voicecall01"]})
        self.run_main()
        self.assertEqual(FakePopen.started, [],
                         "asked ofono before it was there")
        self.ofono_appears()
        self.assertEqual(len(FakePopen.started), 1)

    def test_ofono_going_away_mid_call_lets_go(self):
        self.run_main()
        added, _removed = self.handlers()
        with redirect_stdout(io.StringIO()):
            added(None, None, None, None, None, FakeVariant(["/ril_0/voicecall01"]))
        self.assertEqual(len(FakePopen.started), 1)
        text = self.ofono_vanishes()
        self.assertIn("went away mid-call", text)
        self.assertEqual(self.os.killed, [(FakePopen.PID, sco.signal.SIGTERM)],
                         "a headset left in hands-free waits for a CallRemoved "
                         "that is never coming")

    def test_a_call_left_behind_by_an_ofono_restart_does_not_cost_every_call_after_it(self):
        """The bug this guards: ofono restarting during a call means its
        CallRemoved never arrives. The path stayed in the set, so every later
        call looked like a second call - and a second call is deliberately not
        held. One restart during one call quietly cost every Bluetooth call
        afterwards, until somebody restarted this service."""
        self.run_main()
        added, _removed = self.handlers()
        with redirect_stdout(io.StringIO()):
            added(None, None, None, None, None, FakeVariant(["/ril_0/voicecall01"]))
            self.ofono_vanishes()
            self.ofono_appears()          # ofono is back, with no calls up
            FakePopen.started = []
            # The next call, on a headset, has to be held like any other.
            added(None, None, None, None, None, FakeVariant(["/ril_0/voicecall02"]))
        self.assertEqual(len(FakePopen.started), 1,
                         "the call after an ofono restart was never held")

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
        fake_os = FakeOs()
        sco.os = fake_os
        self.addCleanup(setattr, sco, "os", os_real)
        hold = sco.Hold()
        with redirect_stdout(io.StringIO()):
            hold.start_on("bluez_output.X")
            pid = hold.proc.pid
            hold._too_long()
        self.assertIsNone(hold.proc)
        self.assertEqual(fake_os.killed, [(pid, sco.signal.SIGTERM)])


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



BT_SOURCES = "54\tdroid-sink.monitor\tPipeWire\n55\tdroid-source\tPipeWire\n"
BT_OUTPUTS = (
    "Source Output #359\n\tDriver: PipeWire\n\tSource: 55\n\tProperties:\n"
    "\t\tapplication.name = \"emilia\"\n\n"
    "Source Output #360\n\tDriver: PipeWire\n\tSource: 54\n\tProperties:\n"
    "\t\tapplication.name = \"pavucontrol\"\n\n"
)
BT_CARDS = ("Card #118\n\tName: droid\n\tActive Profile: default\n\n"
            "Card #144\n\tName: bluez_card.F4_9D_8A_7C_5C_66\n"
            "\tActive Profile: a2dp-sink\n")


class BtPactl:
    """pactl, wpctl and audioctl, all of which this service only ever asks."""

    SubprocessError = Exception
    PIPE = -1
    DEVNULL = -3

    def __init__(self, sources=BT_SOURCES, outputs=BT_OUTPUTS, cards=BT_CARDS,
                 setting="Value: true (Saved: true)\n", broken=False):
        self.sources, self.outputs, self.cards = sources, outputs, cards
        self.setting, self.broken = setting, broken
        self.ran = []

    def run(self, argv, **_kwargs):
        argv = list(argv)
        self.ran.append(argv)
        if self.broken:
            raise OSError("pactl is not there")
        if argv[0] == "wpctl":
            return types.SimpleNamespace(stdout=self.setting, returncode=0)
        if argv[0].endswith("audioctl"):
            return types.SimpleNamespace(stdout="bt-mic: headset profile x\n",
                                         returncode=0)
        if "cards" in argv:
            return types.SimpleNamespace(stdout=self.cards, returncode=0)
        if "source-outputs" in argv:
            return types.SimpleNamespace(stdout=self.outputs, returncode=0)
        return types.SimpleNamespace(stdout=self.sources, returncode=0)


class BtMicUses(unittest.TestCase):
    """Shared plumbing: every one of these reads the phone through pactl."""

    def use(self, **kwargs):
        fake = BtPactl(**kwargs)
        btmic.subprocess = fake
        self.addCleanup(setattr, btmic, "subprocess", subprocess_real)
        return fake


class BtMicSeesWhoIsRecording(BtMicUses):
    """Which streams count as somebody wanting a microphone.

    Only droid-source does. A monitor being read is not a recording, and
    switching the headset out of stereo because something watches a level
    meter would be a haunted phone of its own.
    """

    def test_a_recorder_on_the_phone_source_counts(self):
        self.use()
        self.assertEqual(btmic.recorders(), ["emilia"])

    def test_a_stream_on_a_monitor_does_not(self):
        self.use(outputs=BT_OUTPUTS.split("\n\n")[1] + "\n")
        self.assertEqual(btmic.recorders(), [])

    def test_a_recorder_without_a_name_is_still_somebody(self):
        self.use(outputs="Source Output #7\n\tSource: 55\n")
        self.assertEqual(btmic.recorders(), ["something"])

    def test_no_droid_source_means_nothing_to_answer_for(self):
        self.use(sources="54\tdroid-sink.monitor\tPipeWire\n")
        self.assertEqual(btmic.recorders(), [])

    def test_pactl_being_gone_reads_as_nobody_recording(self):
        # The safe direction: no switch, and the phone keeps its own
        # microphone, which is what happened before this service existed.
        self.use(broken=True)
        with redirect_stdout(io.StringIO()):
            self.assertEqual(btmic.recorders(), [])

    def test_the_index_is_read_again_and_not_remembered(self):
        # Indices are not stable - every profile change builds new nodes, and
        # the number that was droid-source can belong to a Bluetooth sink ten
        # minutes later. Same stream, different index, still found.
        self.use(sources="931\tdroid-source\tPipeWire\n",
                 outputs="Source Output #1\n\tSource: 931\n")
        self.assertEqual(btmic.recorders(), ["something"])


class BtMicReadsTheCards(BtMicUses):
    """The phone card is listed first and has a profile of its own.

    Reading that one instead of the headset's is the mistake that would put a
    hold on music Bluetooth during a call - it cost a day once already, in the
    service that holds the link.
    """

    def test_the_headsets_profile_is_the_one_that_is_read(self):
        self.use()
        self.assertFalse(btmic.hands_free())
        self.assertTrue(btmic.headset_connected())

    def test_hands_free_is_seen_for_both_codecs(self):
        for profile in ("headset-head-unit", "headset-head-unit-cvsd"):
            self.use(cards=BT_CARDS.replace("a2dp-sink", profile))
            self.assertTrue(btmic.hands_free(), profile)

    def test_the_phone_cards_profile_is_not_mistaken_for_the_headsets(self):
        self.use(cards=BT_CARDS.replace("Active Profile: default",
                                        "Active Profile: headset-head-unit"))
        self.assertFalse(btmic.hands_free())

    def test_a_call_is_read_from_the_phone_card(self):
        self.use(cards=BT_CARDS.replace("Active Profile: default",
                                        "Active Profile: voicecall"))
        self.assertTrue(btmic.call_is_up())

    def test_no_bluetooth_card_at_all(self):
        self.use(cards="Card #118\n\tName: droid\n\tActive Profile: default\n")
        self.assertFalse(btmic.headset_connected())
        self.assertFalse(btmic.hands_free())

    def test_pactl_being_gone_is_not_a_call(self):
        self.use(broken=True)
        with redirect_stdout(io.StringIO()):
            self.assertFalse(btmic.call_is_up())


class BtMicReadsTheSetting(BtMicUses):
    """Anything that cannot be read counts as off.

    A setting that cannot be read is not a reason to take somebody's music out
    of stereo.
    """

    def test_on(self):
        self.use()
        self.assertTrue(btmic.setting_on())

    def test_off(self):
        self.use(setting="Value: false\n")
        self.assertFalse(btmic.setting_on())

    def test_an_answer_without_a_value_counts_as_off(self):
        self.use(setting="Setting 'furios.bluetooth-mic-routing' not found\n")
        self.assertFalse(btmic.setting_on())

    def test_wpctl_being_gone_counts_as_off(self):
        self.use(broken=True)
        with redirect_stdout(io.StringIO()) as out:
            self.assertFalse(btmic.setting_on())
        self.assertIn("staying out of it", out.getvalue())


class BtMicKnowsAHandHeldHold(unittest.TestCase):
    """Told apart from a leftover by the pid file audioctl writes.

    Somebody who ran "audioctl bt-mic on" in a terminal is holding the headset
    on purpose and must not have it taken away; a headset left hands-free by
    something that is gone has nobody to put it back.
    """

    def setUp(self):
        self.runtime = os.environ.get("XDG_RUNTIME_DIR")
        self.dir = tempfile.mkdtemp()
        os.environ["XDG_RUNTIME_DIR"] = self.dir
        self.addCleanup(self.restore)

    def restore(self):
        if self.runtime is None:
            os.environ.pop("XDG_RUNTIME_DIR", None)
        else:
            os.environ["XDG_RUNTIME_DIR"] = self.runtime
        shutil_real.rmtree(self.dir, ignore_errors=True)

    def write(self, text):
        with open(os.path.join(self.dir, "furios-audio-sco-hold.pid"), "w") as fh:
            fh.write(text)

    def test_no_file_means_nobody_is_holding(self):
        self.assertFalse(btmic.hand_held())

    def test_a_live_pid_means_somebody_is(self):
        self.write("%d\n" % os.getpid())
        self.assertTrue(btmic.hand_held())

    def test_a_pid_that_is_gone_does_not_count(self):
        # The file outlives the process it names: audioctl removes it on the
        # way out, but a kill -9 or a reboot leaves it behind, and a stale
        # file would make every leftover look deliberate forever.
        self.write("999999")
        self.assertFalse(btmic.hand_held())

    def test_junk_in_the_file_does_not_count(self):
        self.write("not a pid")
        self.assertFalse(btmic.hand_held())

    def test_without_a_runtime_dir_there_is_nothing_to_read(self):
        os.environ.pop("XDG_RUNTIME_DIR", None)
        self.assertFalse(btmic.hand_held())


class FakeTimers:
    """GLib's timers, held still so a test can decide when they fire."""

    def __init__(self):
        self.pending = {}
        self.removed = []
        self.next_id = 1

    def timeout_add(self, _ms, fn, *args):
        token = self.next_id
        self.next_id += 1
        self.pending[token] = (fn, args)
        return token

    def source_remove(self, token):
        self.removed.append(token)
        self.pending.pop(token, None)

    def fire_all(self):
        """Every timer that is due, in the order it was asked for."""
        while self.pending:
            token = sorted(self.pending)[0]
            fn, args = self.pending.pop(token)
            fn(*args)


class BtMicSwitching(unittest.TestCase):
    """Taking the headset, giving it back, and never doing either twice."""

    def setUp(self):
        self.pactl = BtPactl()
        btmic.subprocess = self.pactl
        self.addCleanup(setattr, btmic, "subprocess", subprocess_real)
        self.timers = FakeTimers()
        self.glib = btmic.GLib
        btmic.GLib = types.SimpleNamespace(
            timeout_add=self.timers.timeout_add,
            source_remove=self.timers.source_remove)
        self.addCleanup(setattr, btmic, "GLib", self.glib)
        self.free = [False]
        self.real_hands_free = btmic.hands_free
        btmic.hands_free = lambda: self.free[0]
        self.addCleanup(setattr, btmic, "hands_free", self.real_hands_free)
        self.switch = btmic.Switch("/usr/bin/audioctl")

    def audioctl_calls(self):
        return [argv[1:] for argv in self.pactl.ran
                if argv and argv[0].endswith("audioctl")]

    def take(self, who="emilia"):
        out = io.StringIO()
        with redirect_stdout(out):
            self.switch.take(who)
        return out.getvalue()

    def test_it_takes_the_headset_and_holds_it_for_hours_not_minutes(self):
        said = self.take()
        self.assertIn("taking the headset into hands-free", said)
        self.assertEqual(self.audioctl_calls(),
                         [["bt-mic", "on", str(btmic.MAX_HOLD_S)]])
        self.assertTrue(self.switch.ours)

    def test_taking_it_twice_does_nothing_the_second_time(self):
        self.take()
        self.take()
        self.assertEqual(len(self.audioctl_calls()), 1)

    def test_a_headset_already_hands_free_is_left_alone(self):
        self.free[0] = True
        said = self.take()
        self.assertIn("leaving it alone", said)
        self.assertEqual(self.audioctl_calls(), [])
        self.assertFalse(self.switch.ours,
                         "what we did not switch is not ours to undo")

    def test_it_says_that_only_once_per_recording(self):
        # pactl reports several changes for one recording. Eight identical
        # lines per memo bury the one line that matters.
        self.free[0] = True
        self.take()
        self.assertEqual(self.take(), "")

    def test_and_says_it_again_for_the_next_recording(self):
        self.free[0] = True
        self.take()
        with redirect_stdout(io.StringIO()):
            self.switch.release()
        self.assertIn("leaving it alone", self.take())

    def test_a_failed_switch_leaves_nothing_to_undo(self):
        self.pactl.run = lambda argv, **kw: (_ for _ in ()).throw(OSError("no"))
        said = self.take()
        self.assertIn("keeps the phone microphone", said)
        self.assertFalse(self.switch.ours)

    def test_giving_it_back_waits_a_moment_first(self):
        self.take()
        self.switch.release()
        self.assertEqual(len(self.audioctl_calls()), 1,
                         "it must not let go the instant a stream ends")
        with redirect_stdout(io.StringIO()):
            self.timers.fire_all()
        self.assertIn(["bt-mic", "off"], self.audioctl_calls())
        self.assertFalse(self.switch.ours)

    def test_a_second_memo_inside_the_grace_keeps_the_link(self):
        # Rebuilding the link costs about 1.5 s of silence at the start of a
        # recording, measured 2026-09-13. Two memos in a row should not pay it
        # twice.
        self.take()
        self.switch.release()
        said = self.take("emilia")
        self.assertIn("keeping the headset", said)
        self.assertTrue(self.switch.ours)
        self.assertNotIn(["bt-mic", "off"], self.audioctl_calls())

    def test_letting_go_at_once_when_asked_to(self):
        self.take()
        with redirect_stdout(io.StringIO()):
            self.switch.release(grace=False)
        self.assertIn(["bt-mic", "off"], self.audioctl_calls())

    def test_a_grace_timer_that_outlives_its_reason_does_nothing(self):
        # Stopped, or switched off another way, while the two-and-a-half
        # seconds were still running. The timer fires regardless - it must
        # not ask a second time and it must not say it did.
        self.take()
        self.switch.release()
        with redirect_stdout(io.StringIO()):
            self.switch.release(grace=False)
            before = len(self.audioctl_calls())
            self.timers.fire_all()
        self.assertEqual(len(self.audioctl_calls()), before)

    def test_releasing_what_was_never_ours_does_nothing(self):
        with redirect_stdout(io.StringIO()):
            self.switch.release()
        self.assertEqual(self.audioctl_calls(), [])


class BtMicChecksThatItReallyWentBack(unittest.TestCase):
    """Asking is not arriving.

    A card that has lost its A2DP profiles answers "No such entity" and stays
    hands-free. Every track after the memo then plays mono at 16 kHz with
    nothing saying why - seen on 2026-09-13 after a WirePlumber restart left
    the card with only "off" and the two headset profiles.
    """

    def setUp(self):
        self.pactl = BtPactl()
        btmic.subprocess = self.pactl
        self.addCleanup(setattr, btmic, "subprocess", subprocess_real)
        self.timers = FakeTimers()
        self.glib = btmic.GLib
        btmic.GLib = types.SimpleNamespace(
            timeout_add=self.timers.timeout_add,
            source_remove=self.timers.source_remove)
        self.addCleanup(setattr, btmic, "GLib", self.glib)
        self.free = [True]
        self.real = btmic.hands_free
        btmic.hands_free = lambda: self.free[0]
        self.addCleanup(setattr, btmic, "hands_free", self.real)
        self.switch = btmic.Switch("/usr/bin/audioctl")

    def give_back(self):
        out = io.StringIO()
        with redirect_stdout(out):
            self.switch.ours = True
            self.switch._let_go()
            self.timers.fire_all()
        return out.getvalue()

    def offs(self):
        return len([a for a in self.pactl.ran if a[1:3] == ["bt-mic", "off"]])

    def test_a_headset_that_went_back_is_not_asked_again(self):
        self.free[0] = False
        self.give_back()
        self.assertEqual(self.offs(), 1)

    def test_one_that_did_not_is_asked_again_and_then_said_out_loud(self):
        said = self.give_back()
        self.assertEqual(self.offs(), 1 + btmic.VERIFY_TRIES)
        self.assertIn("still in hands-free", said)
        self.assertIn("mono", said)
        self.assertIn("bluetoothctl", said,
                      "a warning without the cure is half a warning")

    def test_a_new_recording_stops_the_checking(self):
        # Switched on again on purpose while the check was pending. Undoing it
        # here would take the headset away from a running memo.
        out = io.StringIO()
        with redirect_stdout(out):
            self.switch.ours = True
            self.switch._let_go()
            self.switch.ours = True
            self.timers.fire_all()
        self.assertEqual(self.offs(), 1)


class FakeSwitch:
    """A headset, as far as the decision table can tell."""

    def __init__(self, ours=False):
        self.ours = ours
        self.taken = []
        self.released = 0
        self.leftovers = 0

    def take(self, who):
        self.taken.append(who)

    def release(self, grace=True):
        self.released += 1

    def give_back_leftover(self):
        self.leftovers += 1


class BtMicDecides(unittest.TestCase):
    """The whole decision table, one line of it at a time."""

    def setUp(self):
        self.world = dict(recorders=["emilia"], call=False, setting=True,
                          headset=True, free=False, held=False)
        originals = {name: getattr(btmic, name) for name in
                     ("recorders", "call_is_up", "setting_on",
                      "headset_connected", "hands_free", "hand_held")}
        self.addCleanup(lambda: [setattr(btmic, k, v)
                                 for k, v in originals.items()])
        btmic.recorders = lambda: self.world["recorders"]
        btmic.call_is_up = lambda: self.world["call"]
        btmic.setting_on = lambda: self.world["setting"]
        btmic.headset_connected = lambda: self.world["headset"]
        btmic.hands_free = lambda: self.world["free"]
        btmic.hand_held = lambda: self.world["held"]
        self.switch = FakeSwitch()
        self.watcher = btmic.Watcher(self.switch)

    def look(self, slow=False):
        with redirect_stdout(io.StringIO()):
            self.watcher.look(slow=slow)

    def test_a_recording_takes_the_headset(self):
        self.look()
        self.assertEqual(self.switch.taken, ["emilia"])

    def test_two_recorders_are_named_once_each(self):
        self.world["recorders"] = ["emilia", "emilia", "signal"]
        self.look()
        self.assertEqual(self.switch.taken, ["emilia, signal"])

    def test_a_call_is_none_of_this_services_business(self):
        # droid-bluetooth-call.lua sets these same ports and
        # furios-audio-sco-hold holds this same link. Two services setting one
        # card is how a call ends up with no audio in either direction.
        self.world["call"] = True
        self.look()
        self.assertEqual(self.switch.taken, [])

    def test_the_setting_has_to_be_on(self):
        self.world["setting"] = False
        self.look()
        self.assertEqual(self.switch.taken, [])

    def test_no_headset_is_nothing_to_switch(self):
        self.world["headset"] = False
        self.look()
        self.assertEqual(self.switch.taken, [])

    def test_nothing_recording_gives_the_headset_back(self):
        self.world["recorders"] = []
        self.look()
        self.assertEqual(self.switch.released, 1)

    def test_a_forgotten_hands_free_headset_is_put_back(self):
        # Something left it this way and is no longer around to undo it - a
        # WirePlumber restart picking the only profile available at that
        # instant, measured 2026-09-13. Nothing else will ever put it back,
        # and what the owner notices is that their music has gone mono.
        self.world.update(recorders=[], free=True)
        self.look(slow=True)
        self.assertEqual(self.switch.leftovers, 0, "not on the first look")
        self.look(slow=True)
        self.assertEqual(self.switch.leftovers, 1)

    def test_an_event_never_concludes_that_on_its_own(self):
        # A call routes the headset before callaudiod has set the phone card's
        # own profile. Undoing that inside the gap would take the call off the
        # headset it was just put on.
        self.world.update(recorders=[], free=True)
        for _ in range(6):
            self.look()
        self.assertEqual(self.switch.leftovers, 0)

    def test_a_hold_somebody_started_by_hand_is_left_alone(self):
        self.world.update(recorders=[], free=True, held=True)
        self.look(slow=True)
        self.look(slow=True)
        self.assertEqual(self.switch.leftovers, 0)

    def test_a_call_on_the_headset_is_left_alone(self):
        self.world.update(recorders=[], free=True, call=True)
        self.look(slow=True)
        self.look(slow=True)
        self.assertEqual(self.switch.leftovers, 0)

    def test_what_we_switched_ourselves_is_not_a_leftover(self):
        self.switch.ours = True
        self.world.update(recorders=[], free=True)
        self.look(slow=True)
        self.look(slow=True)
        self.assertEqual(self.switch.leftovers, 0)

    def test_a_recording_in_between_starts_the_counting_over(self):
        self.world.update(recorders=[], free=True)
        self.look(slow=True)
        self.world["recorders"] = ["emilia"]
        self.look(slow=True)
        self.world["recorders"] = []
        self.look(slow=True)
        self.assertEqual(self.switch.leftovers, 0)


class BtMicNeedsItsTools(unittest.TestCase):
    """Without them it says so and stops, rather than failing per event."""

    def with_tools(self, missing):
        which = btmic.shutil.which
        btmic.shutil.which = lambda tool: None if tool == missing else "/usr/bin/" + tool
        self.addCleanup(setattr, btmic.shutil, "which", which)
        out = io.StringIO()
        with redirect_stdout(out):
            btmic.main()
        return out.getvalue()

    def test_without_pactl(self):
        self.assertIn("pactl is not installed", self.with_tools("pactl"))

    def test_without_wpctl(self):
        self.assertIn("wpctl is not installed", self.with_tools("wpctl"))

    def test_without_audioctl(self):
        # The four steps live in audioctl. Without it this service knows when
        # but not how, and saying so beats switching nothing per event.
        self.assertIn("audioctl is not installed", self.with_tools("audioctl"))

class BtMicPutsBackWhatNobodyElseWill(unittest.TestCase):
    """The leftover path, through the real Switch rather than a stand-in."""

    def setUp(self):
        self.pactl = BtPactl()
        btmic.subprocess = self.pactl
        self.addCleanup(setattr, btmic, "subprocess", subprocess_real)
        self.timers = FakeTimers()
        glib = btmic.GLib
        btmic.GLib = types.SimpleNamespace(
            timeout_add=self.timers.timeout_add,
            source_remove=self.timers.source_remove)
        self.addCleanup(setattr, btmic, "GLib", glib)
        real = btmic.hands_free
        btmic.hands_free = lambda: False
        self.addCleanup(setattr, btmic, "hands_free", real)

    def test_it_says_why_and_hands_the_headset_back(self):
        switch = btmic.Switch("/usr/bin/audioctl")
        out = io.StringIO()
        with redirect_stdout(out):
            switch.give_back_leftover()
            self.timers.fire_all()
        said = out.getvalue()
        self.assertIn("nothing recording", said)
        self.assertNotIn("recording over", said,
                         "nothing was recording - saying so would send the "
                         "next reader looking for a memo that never happened")
        self.assertIn(["bt-mic", "off"],
                      [argv[1:] for argv in self.pactl.ran
                       if argv[0].endswith("audioctl")])
        self.assertFalse(switch.ours)


class BtMicMainWiresItUp(unittest.TestCase):
    """main() only wires things up; the handlers are what decide."""

    def setUp(self):
        self.pactl = BtPactl()
        self.events = types.SimpleNamespace(
            stdout=types.SimpleNamespace(fileno=lambda: 7), poll=lambda: None,
            terminate=lambda: self.terminated.append(True))
        self.terminated = []
        self.pactl.Popen = lambda argv, **kw: self.events
        btmic.subprocess = self.pactl
        self.addCleanup(setattr, btmic, "subprocess", subprocess_real)

        self.quits, self.signals, self.watches, self.timers = [], [], [], []
        originals = (btmic.GLib.MainLoop, btmic.GLib.io_add_watch,
                     btmic.GLib.timeout_add, btmic.GLib.IOChannel,
                     btmic.GLib.IOCondition, btmic.shutil.which,
                     btmic.unix_signal_add, btmic.os.read)

        def restore():
            (btmic.GLib.MainLoop, btmic.GLib.io_add_watch,
             btmic.GLib.timeout_add, btmic.GLib.IOChannel,
             btmic.GLib.IOCondition, btmic.shutil.which,
             btmic.unix_signal_add, btmic.os.read) = originals
        self.addCleanup(restore)

        # The stub fabricates anything asked of it, but these are flags that
        # get masked together - they have to be numbers, not objects.
        btmic.GLib.IOCondition = types.SimpleNamespace(IN=1, ERR=8, HUP=16)

        btmic.GLib.MainLoop = lambda: type(
            "Loop", (), {"run": lambda _s: None,
                         "quit": lambda _s: self.quits.append(True)})()
        btmic.GLib.io_add_watch = lambda ch, prio, cond, fn: self.watches.append(fn)
        btmic.GLib.timeout_add = lambda ms, fn, *a: self.timers.append((ms, fn, a))
        btmic.GLib.IOChannel = types.SimpleNamespace(
            unix_new=lambda fd: types.SimpleNamespace(
                set_encoding=lambda _e: None, set_flags=lambda _f: None,
                unix_get_fd=lambda: fd))
        btmic.shutil.which = lambda tool: "/usr/bin/" + tool
        btmic.unix_signal_add = lambda _p, _s, fn: self.signals.append(fn)
        self.chunks = [b"Event 'new' on source-output #359\n"]
        btmic.os.read = lambda _fd, _n: self.chunks.pop(0) if self.chunks else b""

        self.looks = []
        real = btmic.Watcher.look
        btmic.Watcher.look = lambda _self, slow=False: self.looks.append(slow)
        self.addCleanup(setattr, btmic.Watcher, "look", real)

    def run_main(self):
        out = io.StringIO()
        with redirect_stdout(out):
            btmic.main()
        return out.getvalue()

    def on_event(self, condition=None):
        condition = btmic.GLib.IOCondition.IN if condition is None else condition
        with redirect_stdout(io.StringIO()):
            return self.watches[0](btmic.GLib.IOChannel.unix_new(7), condition)

    def test_it_looks_once_at_the_start(self):
        # A recording already running when this starts - a restart in the
        # middle of a memo - is found rather than waited for.
        self.run_main()
        self.assertEqual(self.looks, [False])

    def test_a_source_output_event_is_a_reason_to_look(self):
        self.run_main()
        self.looks.clear()
        self.assertTrue(self.on_event())
        self.assertEqual(self.looks, [False])

    def test_other_events_are_not(self):
        self.run_main()
        self.looks.clear()
        self.chunks = [b"Event 'change' on sink #54\n"]
        self.on_event()
        self.assertEqual(self.looks, [])

    def test_half_a_line_waits_for_its_other_half(self):
        # readline would block the whole loop here, and with it the timer that
        # gives the headset back.
        self.run_main()
        self.looks.clear()
        self.chunks = [b"Event 'new' on source-", b"output #7\n"]
        self.on_event()
        self.assertEqual(self.looks, [])
        self.on_event()
        self.assertEqual(self.looks, [False])

    def test_nothing_to_read_is_not_an_error(self):
        self.run_main()
        self.chunks = []
        self.assertTrue(self.on_event())

    def test_a_read_that_fails_keeps_the_watch(self):
        self.run_main()

        def boom(_fd, _n):
            raise OSError("try again")
        btmic.os.read = boom
        self.assertTrue(self.on_event())

    def test_the_stream_ending_gives_the_headset_back_and_stops(self):
        # systemd restarts this. A headset left hands-free by a service that
        # is no longer running is nobody's to fix.
        self.run_main()
        gave_back = []
        btmic.Switch.release = lambda _s, grace=True: gave_back.append(grace)
        self.addCleanup(setattr, btmic.Switch, "release",
                        btmic.Switch.__dict__["release"])
        self.assertFalse(self.on_event(btmic.GLib.IOCondition.HUP))
        self.assertEqual(gave_back, [False], "no grace period on the way out")
        self.assertTrue(self.quits)

    def test_the_slow_check_is_the_one_that_may_conclude_things(self):
        self.run_main()
        self.looks.clear()
        slow = [fn for ms, fn, _a in self.timers if ms == btmic.RECHECK_MS]
        self.assertEqual(len(slow), 1)
        self.assertTrue(slow[0]())
        self.assertEqual(self.looks, [True])

    def test_being_stopped_releases_the_headset(self):
        self.run_main()
        gave_back = []
        btmic.Switch.release = lambda _s, grace=True: gave_back.append(grace)
        self.addCleanup(setattr, btmic.Switch, "release",
                        btmic.Switch.__dict__["release"])
        self.assertEqual(len(self.signals), 2, "SIGTERM and SIGINT both")
        with redirect_stdout(io.StringIO()):
            self.signals[0]()
        self.assertEqual(gave_back, [False])
        self.assertTrue(self.terminated, "pactl subscribe has to be stopped too")
        self.assertTrue(self.quits)

    def test_it_says_so_when_it_cannot_subscribe_at_all(self):
        def no(argv, **kw):
            raise OSError("no pactl")
        self.pactl.Popen = no
        self.assertIn("could not subscribe", self.run_main())

    def test_without_the_new_signal_module_the_old_spelling_is_used(self):
        import gi as gi_mod
        saved = sys.modules.pop("gi.repository.GLibUnix", None)
        removed = gi_mod.repository.GLibUnix
        del gi_mod.repository.GLibUnix
        try:
            again = load(ROOT / "tools" / "furios-audio-bt-mic.py", "bt_mic_again")
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
