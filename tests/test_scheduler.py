"""Offline tests for the time-based scheduler (injected clock, no real waiting)."""
import datetime as dt
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mpdm.scheduler import Scheduler, ScheduleRule, rule_fires_between


def _daily(action="resume_all", at="01:00", days=None, enabled=True):
    return ScheduleRule(id="x", action=action, at=at, repeat="daily",
                        days=days or [], enabled=enabled)


def test_daily_fires_on_crossing_only():
    rule = _daily(at="01:00")
    prev = dt.datetime(2026, 10, 21, 0, 59)
    now = dt.datetime(2026, 10, 21, 1, 1)
    assert rule_fires_between(rule, prev, now)          # crosses 01:00
    # Window entirely after the time: must not fire again.
    assert not rule_fires_between(rule, dt.datetime(2026, 10, 21, 1, 1),
                                        dt.datetime(2026, 10, 21, 1, 30))
    # Window entirely before: no fire.
    assert not rule_fires_between(rule, dt.datetime(2026, 10, 21, 0, 0),
                                        dt.datetime(2026, 10, 21, 0, 30))


def test_daily_weekday_filter():
    # 2026-10-21 is a Wednesday (weekday 2).
    wed = _daily(at="01:00", days=[2])
    mon = _daily(at="01:00", days=[0])
    prev = dt.datetime(2026, 10, 21, 0, 59)
    now = dt.datetime(2026, 10, 21, 1, 1)
    assert rule_fires_between(wed, prev, now)
    assert not rule_fires_between(mon, prev, now)


def test_daily_handles_multiday_gap():
    # Scheduler was asleep for >1 day; the 01:00 occurrence must still fire once.
    rule = _daily(at="01:00")
    prev = dt.datetime(2026, 10, 20, 12, 0)
    now = dt.datetime(2026, 10, 21, 2, 0)
    assert rule_fires_between(rule, prev, now)


def test_disabled_rule_never_fires():
    rule = _daily(at="01:00", enabled=False)
    assert not rule_fires_between(rule, dt.datetime(2026, 10, 21, 0, 59),
                                        dt.datetime(2026, 10, 21, 1, 1))


def test_once_rule_fires_in_window():
    rule = ScheduleRule(id="y", action="pause_all", at="2026-10-21T15:30", repeat="once")
    assert rule_fires_between(rule, dt.datetime(2026, 10, 21, 15, 29),
                                    dt.datetime(2026, 10, 21, 15, 31))
    assert not rule_fires_between(rule, dt.datetime(2026, 10, 21, 15, 31),
                                        dt.datetime(2026, 10, 21, 16, 0))


class _Recorder:
    def __init__(self):
        self.calls = []

    def make(self, name):
        return lambda rule: self.calls.append((name, rule.rate_bps))


def _scheduler_with_recorder():
    rec = _Recorder()
    sched = Scheduler(actions={
        "resume_all": rec.make("resume_all"),
        "pause_all": rec.make("pause_all"),
        "set_global_rate": rec.make("set_global_rate"),
    })
    return sched, rec


def test_tick_first_call_does_not_backfire():
    sched, rec = _scheduler_with_recorder()
    sched.add_rule("resume_all", "01:00")
    # First tick only establishes the baseline, even though 01:00 is in the past.
    sched.tick(now=dt.datetime(2026, 10, 21, 10, 0))
    assert rec.calls == []


def test_tick_fires_crossed_rule():
    sched, rec = _scheduler_with_recorder()
    sched.add_rule("resume_all", "01:00")
    sched.add_rule("pause_all", "07:00")
    sched.tick(now=dt.datetime(2026, 10, 21, 0, 59))   # baseline before both
    sched.tick(now=dt.datetime(2026, 10, 21, 1, 1))    # crosses 01:00
    assert [c[0] for c in rec.calls] == ["resume_all"]
    sched.tick(now=dt.datetime(2026, 10, 21, 7, 1))    # crosses 07:00
    assert [c[0] for c in rec.calls] == ["resume_all", "pause_all"]


def test_once_rule_disables_after_firing():
    sched, rec = _scheduler_with_recorder()
    r = sched.add_rule("set_global_rate", "2026-10-21T15:30", repeat="once", rate_bps=1024)
    sched.tick(now=dt.datetime(2026, 10, 21, 15, 29))
    sched.tick(now=dt.datetime(2026, 10, 21, 15, 31))
    assert rec.calls == [("set_global_rate", 1024)]
    assert sched.list_rules()[0].enabled is False
    # A later tick past the time must not fire it again.
    sched.tick(now=dt.datetime(2026, 10, 22, 15, 31))
    assert len(rec.calls) == 1


def test_add_rule_validation():
    sched, _ = _scheduler_with_recorder()
    for bad in [("bogus", "01:00"), ("resume_all", "25:00"), ("resume_all", "1am")]:
        try:
            sched.add_rule(*bad)
        except ValueError:
            pass
        else:
            raise AssertionError(f"expected ValueError for {bad}")


def test_persistence_roundtrip():
    with tempfile.TemporaryDirectory() as d:
        store = os.path.join(d, "schedule.json")
        s1 = Scheduler(actions={}, store=store)
        s1.add_rule("resume_all", "01:00")
        s1.add_rule("pause_all", "07:00", days=[0, 1, 2, 3, 4])
        s2 = Scheduler(actions={}, store=store)
        s2.load()
        rules = s2.list_rules()
        assert len(rules) == 2
        assert {r.action for r in rules} == {"resume_all", "pause_all"}
        assert any(r.days == [0, 1, 2, 3, 4] for r in rules)


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
    print("all scheduler tests passed")
