#!/usr/bin/env python3
"""Warning gate: fail on NEW occurrences of the warnings that matter on the PS3.

The PS3 is LP64 and big-endian, so a few warning classes point at real bugs there:
pointer -> 32-bit int casts (GCC: "loses precision [-fpermissive]"; clang:
-Wpointer-to-int-cast and friends), int -> pointer casts of a different size,
uninitialised use (-Wmaybe-uninitialized / -Wsometimes-uninitialized / -Wuninitialized)
and missing returns (-Wreturn-type). The engine already has many of the casts, so the
known ones are listed in a baseline file and only new ones fail.

Reads compiler output (GCC's build.log from `make -f Makefile.ps3 WARNINGS=1`, or the
clang logs from `PS3_CHECK_VARS=CHECK_WARNINGS=1 PS3_CHECK_KEEP=1 tools/ps3-check-all.sh`).
CI gates the GCC build.log against tools/warning-baseline.txt; the clang checker has its
own baseline, tools/warning-baseline-clang.txt (the messages differ between compilers).

  warning-gate.py check  BASELINE LOG...   exit 1 if a (file, warning, message) occurs
                                           more often than the baseline allows
  warning-gate.py update BASELINE LOG...   write the baseline from the logs

Occurrences are keyed by file, flag and message, not line number, so edits elsewhere in a
file don't trip the gate; the same header warning seen from many translation units counts
once. A baseline whose first line is "# bootstrap" makes `check` report only (exit 0) and
write the baseline it would have used next to it (BASELINE.new) for committing.
With GITHUB_ACTIONS set, new warnings are also printed as ::error annotations.
"""
import collections
import os
import re
import sys

# file:line:col: warning: message [-Wflag]  (GCC and clang)
LINE_RE = re.compile(r"^(?P<file>[^\s:][^:]*):(?P<line>\d+):(?:(?P<col>\d+):)?\s*warning:\s*(?P<msg>.*?)\s*\[(?P<flag>-W[\w\-=+]+|-fpermissive)\]\s*$")

GATED_FLAGS = {
	"-Wpointer-to-int-cast",
	"-Wvoid-pointer-to-int-cast",
	"-Wint-to-pointer-cast",
	"-Wint-to-void-pointer-cast",
	"-Wreturn-type",
	"-Wsometimes-uninitialized",
	"-Wuninitialized",
	"-Wmaybe-uninitialized",
}


def gated(flag, msg):
	if flag in GATED_FLAGS:
		return True
	# GCC downgrades the C++ "pointer cast to a smaller integer" error to this warning.
	return flag == "-fpermissive" and "loses precision" in msg


def norm_path(path):
	"""The path inside Code/ (lower case: the case-includes tree is lower case), or None
	for a file outside the repo (a system header)."""
	p = "/" + path.replace("\\", "/")
	for marker in ("/Code/", "/case-includes/"):
		i = p.rfind(marker)
		if i >= 0:
			return os.path.normpath(p[i + len(marker):]).replace("\\", "/").lower()
	return None


def norm_msg(msg):
	# GCC quotes with ‘’ under UTF-8 locales and '' otherwise.
	return msg.replace("‘", "'").replace("’", "'").strip()


def scan(paths):
	"""-> {key: count} and {key: first (file, line)} for the gated warnings."""
	seen = set()
	counts = collections.Counter()
	where = {}
	for path in paths:
		with open(path, encoding="utf-8", errors="replace") as f:
			for raw in f:
				m = LINE_RE.match(raw.rstrip("\r\n"))
				if not m or not gated(m.group("flag"), m.group("msg")):
					continue
				fpath = norm_path(m.group("file"))
				if fpath is None:
					continue	# system headers
				flag, msg = m.group("flag"), norm_msg(m.group("msg"))
				site = (fpath, m.group("line"), m.group("col"), flag, msg)
				if site in seen:
					continue	# the same header line from another translation unit
				seen.add(site)
				key = (fpath, flag, msg)
				counts[key] += 1
				where.setdefault(key, (m.group("file"), m.group("line")))
	return counts, where


def read_baseline(path):
	counts = collections.Counter()
	bootstrap = False
	if not os.path.exists(path):
		return counts, True
	with open(path, encoding="utf-8") as f:
		for i, raw in enumerate(f):
			line = raw.rstrip("\r\n")
			if i == 0 and line.strip() == "# bootstrap":
				bootstrap = True
			if not line or line.startswith("#"):
				continue
			n, fpath, flag, msg = line.split("\t", 3)
			counts[(fpath, flag, msg)] += int(n)
	return counts, bootstrap


def write_baseline(path, counts):
	with open(path, "w", encoding="utf-8", newline="\n") as f:
		f.write("# tools/warning-gate.py baseline: count<TAB>file<TAB>flag<TAB>message\n")
		f.write("# Known occurrences of the gated warnings (regenerate with: warning-gate.py update).\n")
		for key in sorted(counts):
			f.write("%d\t%s\t%s\t%s\n" % ((counts[key],) + key))


def main(argv):
	if len(argv) < 4 or argv[1] not in ("check", "update"):
		sys.stderr.write(__doc__)
		return 2
	mode, baseline_path, logs = argv[1], argv[2], argv[3:]
	counts, where = scan(logs)
	total = sum(counts.values())
	if mode == "update":
		write_baseline(baseline_path, counts)
		print("wrote %s: %d occurrences in %d entries" % (baseline_path, total, len(counts)))
		return 0

	base, bootstrap = read_baseline(baseline_path)
	by_flag = collections.Counter()
	for (fpath, flag, msg), n in counts.items():
		by_flag[flag] += n
	print("gated warnings: %d (%s)" % (total, ", ".join("%s %d" % kv for kv in sorted(by_flag.items())) or "none"))

	if bootstrap:
		write_baseline(baseline_path + ".new", counts)
		print("baseline %s is a bootstrap placeholder: reporting only. Commit %s.new as %s to arm the gate."
			% (baseline_path, baseline_path, baseline_path))
		if os.environ.get("GITHUB_ACTIONS"):
			print("::warning title=Warning gate not armed::%d gated warnings; commit the %s artifact as %s"
				% (total, os.path.basename(baseline_path) + ".new", baseline_path))
		return 0

	new = []
	for key, n in counts.items():
		extra = n - base.get(key, 0)
		if extra > 0:
			new.append((key, extra))
	fixed = sum(max(0, n - counts.get(key, 0)) for key, n in base.items())

	gh = bool(os.environ.get("GITHUB_ACTIONS"))
	for (fpath, flag, msg), extra in sorted(new):
		f, line = where[(fpath, flag, msg)]
		print("NEW %s: %s [%s]%s" % (fpath, msg, flag, " x%d" % extra if extra > 1 else ""))
		if gh:
			ws = os.environ.get("GITHUB_WORKSPACE", "")
			rel = os.path.relpath(f, ws) if ws and os.path.isabs(f) else f
			print("::error file=%s,line=%s,title=New %s warning::%s" % (rel, line, flag, msg))
	if fixed:
		print("%d baseline occurrences are gone: run `warning-gate.py update` to lower the baseline" % fixed)
	if new:
		print("FAIL: %d new gated warning(s) (baseline %s)" % (sum(e for _, e in new), baseline_path))
		return 1
	print("OK: no new gated warnings")
	return 0


if __name__ == "__main__":
	sys.exit(main(sys.argv))
