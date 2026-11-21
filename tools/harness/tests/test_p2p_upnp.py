"""Internet play's UPnP forwarding (port/linux/src/p2p.c's upnp_thread): a forwarded port is offered among this
machine's addresses and counts its NAT as not strict; a renewal the router refuses (or never answers: a reboot)
withdraws it, so that joiners are not sent to punch a dead port and relays are tried as soon as a strict NAT's
are."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = ["renewal"]

# a fault in p2p.c and the case that must catch it
NEGATIVE_CONTROLS = {
    # a failed renewal keeps the forwarding offered (N9, as it was)
    "renewal-failure-kept": (("\t\tp2p.upnp_forwarded = 0;\n\t}\n\telse\n\t{\n\t\tp2p_log(\"Internet play: UPnP: %s\"",
                              "\t}\n\telse\n\t{\n\t\tp2p_log(\"Internet play: UPnP: %s\""), "renewal"),
}


def generated(fault=None):
    p2p = read("port/linux/src/p2p.c")
    code = "\n\n".join(function(p2p, name) for name in ("upnp_thread", "p2p_local_candidates", "p2p_nat_strict"))
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("p2p_upnp", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("p2p_upnp", generated(fault)), case)
    assert status == CHECK_FAILED, f"p2p.c with a fault ({control}) passed '{case}': the test cannot see it"
