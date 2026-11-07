// SPDX-License-Identifier: GPL-3.0-only
#include "web/web_lines.hpp"
#include <cassert>
#include <iostream>
using namespace prospero::web;

int main() {
    const auto lines = ok_lines("OK a b\nOK c\nEND\n");
    assert(lines.size() == 2 && lines[0] == "a b" && lines[1] == "c");
    assert(ok_lines("ERR unknown\n").empty());
    assert(ok_lines("OK\n").size() == 1);

    const auto stream = fields("uuid=U1 address=192.168.1.20 codec=HEVC hdr=0 mode=1920x1080@60 kbps=20000 secs=5 pads=1 "
                               "role=capture channel=- name=Living room PC",
                               "name");
    assert(stream.at("uuid") == "U1" && stream.at("codec") == "HEVC" && stream.at("mode") == "1920x1080@60");
    assert(stream.at("name") == "Living room PC");
    assert(fields("", "name").empty());

    const auto st = status("running=1 app=PS5 Screen pending=0 address=192.168.1.20 client=Living room PC");
    assert(st.at("running") == "1" && st.at("app") == "PS5 Screen" && st.at("pending") == "0");
    assert(st.at("address") == "192.168.1.20" && st.at("client") == "Living room PC");
    const auto idle = status("running=0 app= pending=2 address= client=");
    assert(idle.at("app").empty() && idle.at("pending") == "2" && idle.at("client").empty());

    const auto dev = device("ABC-1 1 1920x1080@60 Steam Deck");
    assert(dev.at("uuid") == "ABC-1" && dev.at("streaming") == "1" && dev.at("mode") == "1920x1080@60");
    assert(dev.at("name") == "Steam Deck");
    assert(device("ABC-2 0 - Phone").at("mode").empty());

    const auto pair = pairing("3 Living room PC 192.168.1.20");
    assert(pair.at("id") == "3" && pair.at("name") == "Living room PC" && pair.at("address") == "192.168.1.20");
    assert(pairing("4 Phone").at("address").empty());

    const auto user = console_user("1b1d9381 Player One");
    assert(user.at("id") == "1b1d9381" && user.at("name") == "Player One");
    assert(console_user("1b1d9382").at("name").empty());
    const auto profiles = device_profiles("ABC-1 1b1d9381,guest");
    assert(profiles.at("uuid") == "ABC-1" && profiles.at("profiles") == "1b1d9381,guest");

    assert(tail("short", 100) == "short");
    assert(tail("line one\nline two\nline three\n", 15) == "line three\n");

    std::cout << "PASS web_lines_test\n";
}
