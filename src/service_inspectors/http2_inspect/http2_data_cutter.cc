//--------------------------------------------------------------------------
// Copyright (C) 2020-2020 Cisco and/or its affiliates. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------
// http2_data_cutter.cc author Maya Dagon <mdagon@cisco.com>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "http2_data_cutter.h"

#include "service_inspectors/http_inspect/http_flow_data.h"
#include "service_inspectors/http_inspect/http_stream_splitter.h"

#include "http2_dummy_packet.h"
#include "http2_utils.h"

using namespace snort;
using namespace HttpCommon;
using namespace Http2Enums;

Http2DataCutter::Http2DataCutter(Http2FlowData* _session_data, HttpCommon::SourceId src_id) :
    session_data(_session_data),  source_id(src_id)
{ }

// Scan data frame, extract information needed for http scan.
// http scan will need the data only, stripped of padding and header.
bool Http2DataCutter::http2_scan(const uint8_t* data, uint32_t length,
    uint32_t* flush_offset, uint32_t frame_len, uint8_t flags)
{
    *flush_offset = cur_data_offset = cur_data = cur_padding = 0;

    if (frame_bytes_seen == 0)
    {
        frame_length = data_len = frame_len;
        padding_len = data_bytes_read = padding_read = 0;
        frame_flags = flags;
        frame_bytes_seen = cur_data_offset = FRAME_HEADER_LENGTH;
        length -= FRAME_HEADER_LENGTH;
        *flush_offset = FRAME_HEADER_LENGTH;
        data_state = ((frame_flags & PADDED) !=0) ? PADDING_LENGTH : DATA;
    }

    uint32_t cur_pos = leftover_bytes;

    while ((cur_pos < length) && (data_state != FULL_FRAME))
    {
        switch (data_state)
        {
        case PADDING_LENGTH:
            padding_len = *(data + cur_data_offset);

            if (data_len <= padding_len)
            {
                *session_data->infractions[source_id] += INF_PADDING_LEN;
                session_data->events[source_id]->create_event(EVENT_PADDING_LEN);
                return false;
            }
            // FIXIT temporary - till multiple data frames sent to http
            if (data_len == (padding_len + 1))
                return false;
            data_len -= (padding_len + 1);
            data_state = DATA;
            cur_pos++;
            cur_data_offset++;
            break;
        case DATA:
          {
            const uint32_t missing = data_len - data_bytes_read;
            cur_data = ((length - cur_pos) >= missing) ?
                missing : (length - cur_pos);
            data_bytes_read += cur_data;
            cur_pos += cur_data;
            if (data_bytes_read == data_len)
                data_state = padding_len ? PADDING : FULL_FRAME;
            break;
          }
        case PADDING:
          {
            const uint32_t missing = padding_len - padding_read;
            cur_padding = ((length - cur_pos) >= missing) ?
                missing : (length - cur_pos);
            cur_pos += cur_padding;
            padding_read += cur_padding;
            if (padding_read == padding_len)
                data_state = FULL_FRAME;
            break;
          }
        default:
            break;
        }
    }

    frame_bytes_seen += (cur_pos - leftover_bytes);
    session_data->scan_remaining_frame_octets[source_id] = frame_length - frame_bytes_seen;
    *flush_offset += cur_pos;

    return true;
}

// Call http scan. After all data in first frame has been sent, set http2_end_stream flag and send
// zero-length buffer to flush through detection
StreamSplitter::Status Http2DataCutter::http_scan(const uint8_t* data, uint32_t* flush_offset)
{
    StreamSplitter::Status scan_result = StreamSplitter::SEARCH;
    uint32_t http_flush_offset = 0;
    Http2DummyPacket dummy_pkt;
    dummy_pkt.flow = session_data->flow;
    uint32_t unused = 0;

    if (cur_data || leftover_bytes)
    {
        scan_result = session_data->hi_ss[source_id]->scan(&dummy_pkt, data + cur_data_offset,
            cur_data + leftover_bytes, unused, &http_flush_offset);

        if (scan_result == StreamSplitter::FLUSH)
        {
            bytes_sent_http += http_flush_offset;
            leftover_bytes = cur_data + leftover_bytes - http_flush_offset;
            *flush_offset -= leftover_bytes;
            session_data->mid_packet[source_id] = ( leftover_bytes > 0 ) ? true : false;
        }
        else if (scan_result == StreamSplitter::SEARCH)
        {
            bytes_sent_http += (cur_data + leftover_bytes);
            leftover_bytes = 0;
        }
        else if (scan_result == StreamSplitter::ABORT)
            return StreamSplitter::ABORT;
    }
    if (data_state == FULL_FRAME)
    {
        if (leftover_bytes == 0)
        {
            session_data->get_current_stream(source_id)->get_hi_flow_data()->
                set_http2_end_stream(source_id);
            scan_result = session_data->hi_ss[source_id]->scan(&dummy_pkt, nullptr, 0, unused,
                &http_flush_offset);
            assert(scan_result == StreamSplitter::FLUSH);

            // FIXIT-H for now only a single data frame is processed
            Http2Stream* const stream = session_data->find_stream(
                session_data->current_stream[source_id]);
            stream->set_abort_data_processing(source_id);

            // Done with this frame, cleanup
            session_data->mid_packet[source_id] = false;
            session_data->scan_octets_seen[source_id] = 0;
            session_data->scan_remaining_frame_octets[source_id] = 0;
            frame_bytes_seen = 0;
        }
    }

    if (scan_result != StreamSplitter::FLUSH)
        *flush_offset = 0;

    return scan_result;
}

StreamSplitter::Status Http2DataCutter::scan(const uint8_t* data, uint32_t length,
    uint32_t* flush_offset, uint32_t frame_len, uint8_t frame_flags)
{
    // FIXIT-H temporary, until more than 1 data frame sent to http inspect is supported
    Http2Stream* const stream = session_data->find_stream(session_data->current_stream[source_id]);
    if (stream->get_abort_data_processing(source_id))
        return StreamSplitter::ABORT;

    if (!http2_scan(data, length, flush_offset, frame_len, frame_flags))
        return StreamSplitter::ABORT;

    return http_scan(data, flush_offset);
}

const StreamBuffer Http2DataCutter::reassemble(unsigned, const uint8_t* data,
    unsigned len)
{
    StreamBuffer frame_buf { nullptr, 0 };

    cur_data = cur_padding = cur_data_offset = 0;

    unsigned cur_pos = 0;
    while (cur_pos < len)
    {
        switch (reassemble_state)
        {
        case GET_FRAME_HDR:
          {
            if (reassemble_hdr_bytes_read == 0)
            {
                session_data->frame_header[source_id] = new uint8_t[FRAME_HEADER_LENGTH];
                session_data->frame_header_size[source_id] = FRAME_HEADER_LENGTH;
                padding_len = 0;
            }

            const uint32_t missing = FRAME_HEADER_LENGTH - reassemble_hdr_bytes_read;
            const uint32_t cur_frame = ((len - cur_pos) < missing) ? (len - cur_pos) : missing;
            memcpy(session_data->frame_header[source_id] + reassemble_hdr_bytes_read, data +
                cur_pos,
                cur_frame);
            reassemble_hdr_bytes_read += cur_frame;
            cur_pos += cur_frame;

            if (reassemble_hdr_bytes_read == FRAME_HEADER_LENGTH)
            {
                data_len = frame_length = get_frame_length(session_data->frame_header[source_id]);
                frame_flags = get_frame_flags(session_data->frame_header[source_id]);
                cur_data_offset = cur_pos;
                reassemble_state = ((frame_flags & PADDED) !=0) ? GET_PADDING_LEN : SEND_DATA;
            }

            break;
          }
        case GET_PADDING_LEN:
            padding_len = *(data + cur_pos);
            data_len -= (padding_len + 1);
            cur_pos++;
            cur_data_offset++;
            reassemble_state = SEND_DATA;
            break;
        case SEND_DATA:
          {
            const uint32_t missing = data_len - reassemble_data_bytes_read;
            cur_data = ((len - cur_pos) >= missing) ? missing : (len - cur_pos);
            reassemble_data_bytes_read += cur_data;
            cur_pos += cur_data;

            unsigned copied;
            uint32_t flags = (bytes_sent_http == (cur_data + reassemble_bytes_sent)) ?
                PKT_PDU_TAIL : 0;
            frame_buf = session_data->hi_ss[source_id]->reassemble(session_data->flow,
                bytes_sent_http, 0, data + cur_data_offset, cur_data,
                flags, copied);
            assert(copied == (unsigned)cur_data);
            reassemble_bytes_sent += copied;

            if (reassemble_data_bytes_read == data_len)
                reassemble_state = (padding_len) ? SKIP_PADDING : CLEANUP;

            break;
          }
        case SKIP_PADDING:
          {
            const uint32_t missing = padding_len - reassemble_padding_read;
            cur_padding = ((len - cur_pos) >= missing) ?
                missing : (len - cur_pos);
            cur_pos += cur_padding;
            reassemble_padding_read += cur_padding;
            if (reassemble_padding_read == padding_len)
                reassemble_state = CLEANUP;
            break;
          }

        default:
            break;
        }
    }

    if (reassemble_state == CLEANUP)
    {
        // Done with this packet
        reassemble_state = GET_FRAME_HDR;
        reassemble_hdr_bytes_read = reassemble_data_bytes_read = reassemble_padding_read = 0;
    }

    if (frame_buf.data != nullptr)
    {
        session_data->frame_data[source_id] = const_cast <uint8_t*>(frame_buf.data);
        session_data->frame_data_size[source_id] = frame_buf.length;
        bytes_sent_http = reassemble_bytes_sent = 0;
    }

    return frame_buf;
}

