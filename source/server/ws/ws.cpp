/*!
    \file ws.cpp
    \brief WebSocket C++ Library implementation
    \author Ivan Shynkarenka
    \date 22.05.2019
    \copyright MIT License
*/

#include "server/ws/ws.h"

#include "string/encoding.h"
#include "string/format.h"

#include <algorithm>
#include <openssl/sha.h>

namespace CppServer {
namespace WS {

bool WebSocket::PerformUpgrade(const HTTP::HTTPRequest& request)
{
    return true;
}

bool WebSocket::PerformUpgrade(const HTTP::HTTPResponse& response, const CppCommon::UUID& id)
{
    // Try to perform WebSocket handshake
    if (response.status() == 101)
    {
        bool error = false;
        bool accept = false;
        bool connection = false;
        bool upgrade = false;

        for (size_t i = 0; i < response.headers(); ++i)
        {
            auto header = response.header(i);
            auto key = std::get<0>(header);
            auto value = std::get<1>(header);

            if (key == "Connection")
            {
                if (value != "Upgrade")
                {
                    error = true;
                    onWSError("Invalid WebSocket handshaked response: 'Connection' header value must be 'Upgrade'");
                    break;
                }

                connection = true;
            }
            else if (key == "Upgrade")
            {
                if (value != "websocket")
                {
                    error = true;
                    onWSError("Invalid WebSocket handshaked response: 'Upgrade' header value must be 'websocket'");
                    break;
                }

                upgrade = true;
            }
            else if (key == "Sec-WebSocket-Accept")
            {
                // Calculate the original WebSocket hash
                std::string wskey = CppCommon::Encoding::Base64Encode(id.string()) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
                char wshash[SHA_DIGEST_LENGTH];
                SHA1((const unsigned char*)wskey.data(), wskey.size(), (unsigned char*)wshash);

                // Get the received WebSocket hash
                wskey = CppCommon::Encoding::Base64Decode(value);

                // Compare original and received hashes
                if (std::strncmp(wskey.data(), wshash, std::max(wskey.size(), sizeof(wshash))) != 0)
                {
                    error = true;
                    onWSError("Invalid WebSocket handshaked response: 'Sec-WebSocket-Accept' value validation failed");
                    break;
                }

                accept = true;
            }
        }

        // Failed to perfrom WebSocket handshake
        if (!accept || !connection || !upgrade)
        {
            if (!error)
                onWSError("Invalid WebSocket response");
            return false;
        }

        // WebSocket successfully handshaked!
        _handshaked = true;
        *((uint32_t*)_ws_send_mask) = rand();
        onWSConnected(response);

        return true;
    }

    onWSError("Invalid WebSocket response status: {}"_format(response.status()));
    return false;
}

void WebSocket::PrepareSendFrame(uint8_t opcode, const void* buffer, size_t size, int status)
{
    // Clear the previous WebSocket send buffer
    _ws_send_buffer.clear();

    // Append WebSocket frame opcode
    _ws_send_buffer.push_back(opcode);

    // Append WebSocket frame size
    if (size <= 125)
        _ws_send_buffer.push_back((size & 0xFF) | 0x80);
    else if (size <= 65535)
    {
        _ws_send_buffer.push_back(126 | 0x80);
        _ws_send_buffer.push_back((size >> 8) & 0xFF);
        _ws_send_buffer.push_back(size & 0xFF);
    }
    else
    {
        _ws_send_buffer.push_back(127 | 0x80);
        for (int i = 7; i >= 0; --i)
            _ws_send_buffer.push_back((size >> (8 * i)) & 0xFF);
    }

    // Append WebSocket frame mask
    _ws_send_buffer.push_back(_ws_send_mask[0]);
    _ws_send_buffer.push_back(_ws_send_mask[1]);
    _ws_send_buffer.push_back(_ws_send_mask[2]);
    _ws_send_buffer.push_back(_ws_send_mask[3]);

    // Resize WebSocket frame buffer
    size_t offset = _ws_send_buffer.size();
    _ws_send_buffer.resize(offset + size);

    // Mask WebSocket frame content
    const uint8_t* data = (const uint8_t*)buffer;
    for (size_t i = 0; i < size; ++i)
        _ws_send_buffer[offset + i] = data[i] ^ _ws_send_mask[i % 4];
}

size_t WebSocket::RequiredReceiveFrameSize()
{
    if (_ws_received)
        return 0;

    // Required WebSocket frame opcode and mask flag
    if (_ws_receive_buffer.size() < 2)
        return 2 - _ws_receive_buffer.size();

    bool mask = ((_ws_receive_buffer[1] >> 7) & 0x01) != 0;
    size_t payload = _ws_receive_buffer[1] & (~0x80);

    // Required WebSocket frame size
    if ((payload == 126) && (_ws_receive_buffer.size() < 4))
        return 4 - _ws_receive_buffer.size();
    if ((payload == 127) && (_ws_receive_buffer.size() < 10))
        return 10 - _ws_receive_buffer.size();

    // Required WebSocket frame mask
    if ((mask) && (_ws_receive_buffer.size() < _ws_header_size))
        return _ws_header_size - _ws_receive_buffer.size();

    // Required WebSocket frame payload
    return _ws_header_size + _ws_payload_size - _ws_receive_buffer.size();
}

void WebSocket::PrepareReceiveFrame(const void* buffer, size_t size)
{
    const uint8_t* data = (const uint8_t*)buffer;

    // Clear received data after WebSocket frame was processed
    if (_ws_received)
    {
        _ws_received = false;
        _ws_header_size = 0;
        _ws_payload_size = 0;
        _ws_receive_buffer.clear();
        *((uint32_t*)_ws_receive_mask) = 0;
    }

    while (size > 0)
    {
        // Prepare WebSocket frame opcode and mask flag
        if (_ws_receive_buffer.size() < 2)
        {
            for (size_t i = 0; i < 2; ++i, ++data, --size)
            {
                if (size == 0)
                    return;
                _ws_receive_buffer.push_back(*data);
            }
        }

        [[maybe_unused]] uint8_t opcode = _ws_receive_buffer[0] & 0x0F;
        [[maybe_unused]] bool fin = ((_ws_receive_buffer[0] >> 7) & 0x01) != 0;
        bool mask = ((_ws_receive_buffer[1] >> 7) & 0x01) != 0;
        size_t payload = _ws_receive_buffer[1] & (~0x80);

        // Prepare WebSocket frame size
        if (payload <= 125)
        {
            _ws_header_size = 2 + (mask ? 4 : 0);
            _ws_payload_size = payload;
            _ws_receive_buffer.reserve(_ws_header_size + _ws_payload_size);
        }
        else if (payload == 126)
        {
            if (_ws_receive_buffer.size() < 4)
            {
                for (size_t i = 0; i < 2; ++i, ++data, --size)
                {
                    if (size == 0)
                        return;
                    _ws_receive_buffer.push_back(*data);
                }
            }

            payload = (((size_t)_ws_receive_buffer[2] << 8) | ((size_t)_ws_receive_buffer[3] << 0));
            _ws_header_size = 4 + (mask ? 4 : 0);
            _ws_payload_size = payload;
            _ws_receive_buffer.reserve(_ws_header_size + _ws_payload_size);
        }
        else if (payload == 127)
        {
            if (_ws_receive_buffer.size() < 10)
            {
                for (size_t i = 0; i < 8; ++i, ++data, --size)
                {
                    if (size == 0)
                        return;
                    _ws_receive_buffer.push_back(*data);
                }
            }

            payload = (((size_t)_ws_receive_buffer[2] << 56) | ((size_t)_ws_receive_buffer[3] << 48) | ((size_t)_ws_receive_buffer[4] << 40) | ((size_t)_ws_receive_buffer[5] << 32) | ((size_t)_ws_receive_buffer[6] << 24) | ((size_t)_ws_receive_buffer[7] << 16) | ((size_t)_ws_receive_buffer[8] << 8) | ((size_t)_ws_receive_buffer[9] << 0));
            _ws_header_size = 10 + (mask ? 4 : 0);
            _ws_payload_size = payload;
            _ws_receive_buffer.reserve(_ws_header_size + _ws_payload_size);
        }

        // Prepare WebSocket frame mask
        if (mask)
        {
            if (_ws_receive_buffer.size() < _ws_header_size)
            {
                for (size_t i = 0; i < 4; ++i, ++data, --size)
                {
                    if (size == 0)
                        return;
                    _ws_receive_mask[i] = *data;
                }
            }
        }

        size_t total = _ws_header_size + _ws_payload_size;
        size_t length = std::min(total - _ws_receive_buffer.size(), size);

        // Prepare WebSocket frame payload
        _ws_receive_buffer.insert(_ws_receive_buffer.end(), data, data + length);
        data += length;
        size -= length;

        // Process WebSocket frame
        if (_ws_receive_buffer.size() == total)
        {
            size_t offset = _ws_header_size;

            // Unmask WebSocket frame content
            if (mask)
                for (size_t i = 0; i < _ws_payload_size; ++i)
                    _ws_receive_buffer[offset + i] ^= _ws_receive_mask[i % 4];

            _ws_received = true;

            // Call the WebSocket received handler
            onWSReceived(_ws_receive_buffer.data() + offset, _ws_payload_size);
        }
    }
}

void WebSocket::ClearWSBuffers()
{
    _ws_received = false;
    _ws_header_size = 0;
    _ws_payload_size = 0;
    _ws_receive_buffer.clear();
    *((uint32_t*)_ws_receive_mask) = 0;

    std::scoped_lock locker(_ws_send_lock);

    _ws_send_buffer.clear();
    *((uint32_t*)_ws_send_mask) = 0;
}

} // namespace WS
} // namespace CppServer
