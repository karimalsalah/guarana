#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The GPU family of a device report: the class the engine's game presets match on.

The engine puts an integrated GPU in a family by its PCI vendor and device ID and, where one ID covers
several parts, by the name its driver reports (Common::ClassifyGpuFamily, src/common/device_profile.cpp).
A report is evidence for a family only when it names its GPU exactly enough that the engine would put
that GPU in that family:

- a PCI ID (a tools/bench row's `gpus[].pci`, `8086:7D51` or `PCI\\VEN_8086&DEV_7D51` on the launcher's
  "GPU:" line): the engine's own table, name part included;
- the GPU's name as its driver reports it (a tools/bench row on Windows, or the launcher's "GPU: <name>."
  line in "Anything else"), when the name holds a part only one family has (140T, 140V, B390, 780M, 890M,
  ...). A name with the parts of two families ("Arc 140T / 140V") and an AMD chip name as lspci prints it
  (tools/bench on Linux) are unresolved;
- nothing else. The form's "Graphics card" answer only narrows the candidates: "Intel Arc 140T / 140V"
  is two families, Arrow Lake-H and Lunar Lake.

So a report gets one of three answers (`status`): a family; `none`, its GPU is in no family (a discrete
card, an older or smaller integrated GPU, or a name the engine does not match); or `unresolved`, with
the families it may be (`candidates`; it may also be in none). Nothing is guessed from the CPU or the
device model. Reports do not say which driver ran: the vendor's own is assumed, as the engine requires.

tests/test_device_classes.py checks FAMILY_ROWS and FAMILIES against device_profile.cpp. Standard
library only.
"""

import re

INTEL = 0x8086
AMD = 0x1002

# Common::GpuFamilyName's names in the engine's order, with a label for people.
FAMILIES = {
    "IntelArcMeteorLake": "Intel Arc Graphics (Meteor Lake)",
    "IntelArrowLakeH": "Intel Arc 140T / 130T (Arrow Lake-H)",
    "IntelArcLunarLake": "Intel Arc 140V / 130V (Lunar Lake)",
    "IntelGraphicsPantherLake": "Intel Graphics (Panther Lake, 4 Xe3 cores or fewer)",
    "IntelArcB370": "Intel Arc B370 (Panther Lake)",
    "IntelArcB390": "Intel Arc B390 (Panther Lake)",
    "AmdRadeon780M": "AMD Radeon 780M",
    "AmdRadeon880M": "AMD Radeon 880M",
    "AmdRadeon890M": "AMD Radeon 890M",
    "AmdRadeon8060S": "AMD Radeon 8060S",
    "AmdCustomGpu0405": "Steam Deck LCD (AMD Custom GPU 0405)",
    "AmdCustomGpu0932": "Steam Deck OLED (AMD Custom GPU 0932)",
}

# The engine's FamilyRows: (PCI vendor ID, PCI device ID, part of the name the device must report, or
# "" when the ID alone decides, family).
FAMILY_ROWS = (
    (INTEL, 0x7D55, "", "IntelArcMeteorLake"),
    (INTEL, 0x7D51, "", "IntelArrowLakeH"),
    (INTEL, 0x7DD1, "140T", "IntelArrowLakeH"),
    (INTEL, 0x7DD1, "130T", "IntelArrowLakeH"),
    (INTEL, 0x64A0, "", "IntelArcLunarLake"),
    (INTEL, 0x64B0, "140V", "IntelArcLunarLake"),
    (INTEL, 0x64B0, "130V", "IntelArcLunarLake"),
    (INTEL, 0xB090, "", "IntelGraphicsPantherLake"),
    (INTEL, 0xB0A0, "", "IntelGraphicsPantherLake"),
    (INTEL, 0xB081, "", "IntelArcB370"),
    (INTEL, 0xB083, "", "IntelArcB370"),
    (INTEL, 0xB085, "", "IntelArcB370"),
    (INTEL, 0xB087, "", "IntelArcB370"),
    (INTEL, 0xB080, "", "IntelArcB390"),
    (INTEL, 0xB082, "", "IntelArcB390"),
    (INTEL, 0xB084, "", "IntelArcB390"),
    (INTEL, 0xB086, "", "IntelArcB390"),
    (AMD, 0x15BF, "780M", "AmdRadeon780M"),
    (AMD, 0x1900, "780M", "AmdRadeon780M"),
    (AMD, 0x150E, "880M", "AmdRadeon880M"),
    (AMD, 0x150E, "890M", "AmdRadeon890M"),
    (AMD, 0x1586, "8060S", "AmdRadeon8060S"),
    (AMD, 0x163F, "", "AmdCustomGpu0405"),
    (AMD, 0x1435, "", "AmdCustomGpu0932"),
)

# Name parts that pin one family whatever the device ID: every GPU a driver names this way has an ID
# of that family (Intel's 140T and 130T are Arrow Lake-H's 7D51 and 7DD1, 140V and 130V Lunar Lake's
# 64A0 and 64B0; AMD's names are the name parts of the engine's rows).
_NAME_PARTS = (
    (INTEL, re.compile(r"\b1[34]0t\b"), "IntelArrowLakeH"),
    (INTEL, re.compile(r"\b1[34]0v\b"), "IntelArcLunarLake"),
    (INTEL, re.compile(r"\bb370\b"), "IntelArcB370"),
    (INTEL, re.compile(r"\bb390\b"), "IntelArcB390"),
    (AMD, re.compile(r"\b780m\b"), "AmdRadeon780M"),
    (AMD, re.compile(r"\b880m\b"), "AmdRadeon880M"),
    (AMD, re.compile(r"\b890m\b"), "AmdRadeon890M"),
    (AMD, re.compile(r"\b8060s\b"), "AmdRadeon8060S"),
    (AMD, re.compile(r"\bcustom gpu 0405\b"), "AmdCustomGpu0405"),
    (AMD, re.compile(r"\bcustom gpu 0932\b"), "AmdCustomGpu0932"),
)
# Names of GPUs outside every family: Intel's discrete Arc A and B cards and its older integrated
# graphics. AMD's ID-only families are the Steam Decks, whose drivers name them "AMD Custom GPU", so
# an AMD name without a family's part is in none.
_INTEL_OUTSIDE = re.compile(r"\b(?:iris|uhd|hd graphics|a\d{3}m?|b[5-9]\d{2})\b")
_VENDOR_WORDS = ((INTEL, re.compile(r"\bintel\b")), (AMD, re.compile(r"\b(?:amd|radeon|ati)\b")),
                 (0x10DE, re.compile(r"\b(?:nvidia|geforce|quadro|rtx|gtx)\b")),
                 (0x5143, re.compile(r"\b(?:qualcomm|adreno|snapdragon)\b")))
# A name as lspci prints it (tools/bench on Linux): the PCI database's, not the driver's.
_LSPCI = re.compile(r"(?i)\(rev [0-9a-f]{2}\)|\bcorporation\b|advanced micro devices|\[amd/ati\]")
VENDOR_IDS = {"intel": INTEL, "amd": AMD, "nvidia": 0x10DE, "qualcomm": 0x5143}

# The device report form's "Graphics card" options (issue_forms.GPU_OPTIONS short names) that are
# integrated GPUs, with the families each may be. Options not listed here are discrete or unknown.
OPTION_CANDIDATES = {
    "Intel Arc 140T / 140V": ("IntelArrowLakeH", "IntelArcLunarLake"),
    "Intel Arc 130T / 130V": ("IntelArrowLakeH", "IntelArcLunarLake"),
    "Intel Arc (Meteor Lake)": ("IntelArcMeteorLake",),
    "Intel Arc (Panther Lake)": ("IntelGraphicsPantherLake", "IntelArcB370", "IntelArcB390"),
    "AMD Radeon 780M / 760M": ("AmdRadeon780M",),
    "AMD Radeon 880M / 890M": ("AmdRadeon880M", "AmdRadeon890M"),
    "AMD Radeon 8060S": ("AmdRadeon8060S",),
    "AMD Ryzen Z1 / Z2": (),
    "Steam Deck": ("AmdCustomGpu0405", "AmdCustomGpu0932"),
    "AMD integrated": (),
}
# Integrated options whose GPUs are in no family: Iris Xe and UHD graphics, and every ARM GPU (the
# engine's families are Intel and AMD only).
OPTION_OUTSIDE = ("Intel Iris Xe / UHD", "Qualcomm Adreno X1 / X2", "Qualcomm Adreno 8xx", "Qualcomm Adreno 7xx",
                  "Qualcomm Adreno 6xx or older", "Android GPU")

FROM_PCI = "PCI ID"
FROM_BENCH = "GPU name in a tools/bench row"
FROM_REPORT = "GPU name in the report"
FROM_ANSWER = "Graphics card answer"

_PCI = (re.compile(r"(?i)\bVEN_([0-9a-f]{4})&DEV_([0-9a-f]{4})"),
        re.compile(r"(?i)(?<![0-9a-z:])(8086|1002|10de):([0-9a-f]{4})(?![0-9a-z:])"))
# The launcher's line in "Anything else" ("GPU: Intel(R) Arc(TM) 140T GPU (16GB). Display ..."), with an
# optional PCI ID in brackets after the name.
_GPU_LINE = re.compile(r"(?im)(?<![\w-])GPU:[ \t]*(?P<name>[^\n\[]{2,100}?)[ \t]*(?:\[(?P<pci>[^\]\n]{4,40})\][ \t]*)?"
                       r"(?:\.(?=[ \t]|$)|$)")


def normalize_name(name):
    """A GPU name for matching: lower case, no trademark signs, words split on anything else."""
    name = re.sub(r"(?i)\((?:r|tm)\)|[®™]", " ", str(name or ""))
    return " ".join(re.findall(r"[a-z0-9]+", name.lower()))


def name_vendor(name):
    """The PCI vendor ID a GPU name names, or None."""
    text = normalize_name(name)
    if re.search(r"\barc\b", text) and not re.search(r"\b(?:amd|radeon|nvidia)\b", text):
        return INTEL
    return next((vendor for vendor, words in _VENDOR_WORDS if words.search(text)), None)


def parse_pci_field(value):
    """(vendor ID, device ID) of a tools/bench row's gpus[].pci ("8086:7D51"), or None."""
    match = re.fullmatch(r"([0-9A-Fa-f]{4}):([0-9A-Fa-f]{4})", str(value or ""))
    return (int(match.group(1), 16), int(match.group(2), 16)) if match else None


def parse_pci(text):
    """(vendor ID, device ID) of the first PCI ID in text, or None."""
    for pattern in _PCI:
        match = pattern.search(str(text or ""))
        if match:
            return int(match.group(1), 16), int(match.group(2), 16)
    return None


def report_gpu_line(text):
    """(name, (vendor, device) or None) from a "GPU: <name>." line, as the launcher writes it into
    "Anything else"; None without one, or when the line names no GPU vendor and has no PCI ID ("GPU:
    68 °C" is a temperature)."""
    for match in _GPU_LINE.finditer(str(text or "")):
        name = match.group("name").strip()
        pci = parse_pci(match.group("pci")) if match.group("pci") else None
        if pci or name_vendor(name) is not None:
            return name, pci
    return None


def result(status, family=None, candidates=(), source=None, name=None):
    return {"family": family, "status": status, "candidates": list(candidates), "from": source, "name": name}


def from_pci(vendor_id, device_id, name=""):
    """The engine's answer for a PCI ID and the driver's name, as status and family."""
    text = str(name or "")
    rows = [row for row in FAMILY_ROWS if row[0] == vendor_id and row[1] == device_id]
    for _, _, part, family in rows:
        if not part or part in text:
            return "family", family, ()
    if rows and not normalize_name(name):
        return "unresolved", None, tuple(dict.fromkeys(row[3] for row in rows))
    return "none", None, ()


def _id_only_families(vendor):
    return tuple(dict.fromkeys(family for row_vendor, _, part, family in FAMILY_ROWS
                               if row_vendor == vendor and not part))


def from_name(name):
    """The family a GPU name pins, by the name alone, as status, family and candidates."""
    text = normalize_name(name)
    vendor = name_vendor(name)
    matched = list(dict.fromkeys(family for part_vendor, pattern, family in _NAME_PARTS
                                 if vendor == part_vendor and pattern.search(text)))
    if len(matched) == 1:
        return "family", matched[0], ()
    if matched:
        # "Intel Arc 140T / 140V", "Radeon 880M / 890M": a label of several GPUs, not one GPU's name.
        return "unresolved", None, tuple(matched)
    if vendor == INTEL and not _INTEL_OUTSIDE.search(text):
        # "Intel(R) Graphics" or "Intel(R) Arc(TM) Graphics": the families matched by their ID alone.
        return "unresolved", None, _id_only_families(INTEL)
    if vendor == AMD and _LSPCI.search(str(name or "")):
        # lspci's name for the chip ("Phoenix1 (rev c1)") is not the driver's, which the engine matches.
        return "unresolved", None, tuple(dict.fromkeys(row[3] for row in FAMILY_ROWS if row[0] == AMD))
    return "none", None, ()


def classify(option=None, vendor=None, kind=None, bench_gpus=(), report_gpu=None, report_pci=None):
    """The family of one report.

    option, vendor, kind: the form's "Graphics card" answer as its short name, vendor (intel, amd,
    nvidia, other) and kind (integrated, discrete, unknown). bench_gpus: the GPUs of its chosen tools/bench
    row, as names or as {"name", "pci": (vendor, device) or None}. report_gpu, report_pci: a GPU name and
    (vendor, device) the report states (the launcher's "GPU:" line)."""
    if kind == "discrete":
        return result("none", source=FROM_ANSWER)
    wanted = VENDOR_IDS.get(vendor)
    sources = []
    for gpu in bench_gpus or ():
        name, pci = (gpu.get("name") or "", gpu.get("pci")) if isinstance(gpu, dict) else (gpu, None)
        if pci or normalize_name(name):
            sources.append((FROM_BENCH, name, pci))
    if report_gpu or report_pci:
        sources.append((FROM_REPORT, report_gpu or "", report_pci))
    answers = []
    for source, name, pci in sources:
        if pci:
            if wanted and pci[0] != wanted:
                continue
            status, family, candidates = from_pci(pci[0], pci[1], name)
            source = FROM_PCI
        else:
            if wanted and name_vendor(name) not in (wanted, None):
                continue
            status, family, candidates = from_name(name)
        answers.append((source, name, status, family, candidates))
    if kind == "integrated":
        # A machine with an integrated and a discrete GPU of one vendor ran the integrated one.
        answers = [answer for answer in answers if answer[2] != "none"] or answers
    if answers:
        decided = {(status, family) for _, _, status, family, _ in answers}
        source, name, status, family, _ = answers[0]
        if len(decided) == 1 and status != "unresolved":
            return result(status, family, (), source, clean_name(name))
        # Names that disagree, or one that fits several families: the families they may be, narrowed
        # by the form's answer where that keeps any.
        candidates = list(dict.fromkeys(candidate for _, _, _, family, more in answers
                                        for candidate in ((family,) if family else more)))
        narrowed = [family for family in candidates if family in OPTION_CANDIDATES.get(option, ())]
        return result("unresolved", None, narrowed or candidates, source, clean_name(name))
    if option in OPTION_OUTSIDE:
        return result("none", source=FROM_ANSWER)
    return result("unresolved", None, OPTION_CANDIDATES.get(option, ()), FROM_ANSWER)


def clean_name(name):
    """The GPU name as a record keeps it: no trademark signs or memory size, at most 60 characters."""
    name = re.sub(r"\((?:R|TM)\)|[®™]", "", str(name or ""), flags=re.I)
    name = re.sub(r"(?i)\(\s*\d+\s*[GM]B\s*\)", "", name)
    return re.sub(r"\s+", " ", name).strip()[:60] or None


def label(family):
    return FAMILIES.get(family, family)
