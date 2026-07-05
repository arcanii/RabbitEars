// SPDX-License-Identifier: GPL-3.0-or-later
// Programme — one XMLTV `<programme>` entry: a single show on one channel over a
// time span. Parsed from an EPG feed (see core/XmltvParser) and stored in the
// epg_programmes table, joined back to a Channel via channelId == Channel.tvgId.
#pragma once

#include <string>

namespace rabbitears {

struct Programme {
    std::wstring channelId;    // XMLTV @channel — matches Channel.tvgId (the EPG join key)
    std::wstring title;        // <title>
    std::wstring subTitle;     // <sub-title> (episode name, etc.)
    std::wstring descr;        // <desc>
    std::wstring category;     // first <category>
    std::wstring episodeNum;   // <episode-num> verbatim
    std::wstring iconUrl;      // <icon src>
    long long    startUtc = 0; // @start as unix epoch seconds (UTC); 0 == unknown
    long long    stopUtc = 0;  // @stop  as unix epoch seconds (UTC); 0 == unknown

    // Valid enough to store: a channel to hang it on and a start time.
    bool isValid() const { return !channelId.empty() && startUtc != 0; }
};

}  // namespace rabbitears
