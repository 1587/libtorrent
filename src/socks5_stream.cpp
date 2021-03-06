/*

Copyright (c) 2007, Arvid Norberg
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include "libtorrent/pch.hpp"

#include "libtorrent/socks5_stream.hpp"
#include "libtorrent/assert.hpp"

namespace libtorrent
{

	socks_error_category socks_category;

	const char* socks_error_category::name() const
	{
		return "socks error";
	}

	std::string socks_error_category::message(int ev) const
	{
		static char const* messages[] =
		{
			"no error",
			"unsupported version",
			"unsupported authentication method",
			"unsupported authentication version",
			"authentication error",
			"username required",
			"general failure",
			"command not supported",
			"no identd running",
			"identd could not identify username"
		};

		if (ev < 0 || ev > socks_error::num_errors) return "unknown error";
		return messages[ev];
	}

	void socks5_stream::name_lookup(error_code const& e, tcp::resolver::iterator i
		, boost::shared_ptr<handler_type> h)
	{
		if (e || i == tcp::resolver::iterator())
		{
			(*h)(e);
			error_code ec;
			close(ec);
			return;
		}

		m_sock.async_connect(i->endpoint(), boost::bind(
			&socks5_stream::connected, this, _1, h));
	}

	void socks5_stream::connected(error_code const& e, boost::shared_ptr<handler_type> h)
	{
		if (e)
		{
			(*h)(e);
			error_code ec;
			close(ec);
			return;
		}

		using namespace libtorrent::detail;
		if (m_version == 5)
		{
			// send SOCKS5 authentication methods
			m_buffer.resize(m_user.empty()?3:4);
			char* p = &m_buffer[0];
			write_uint8(5, p); // SOCKS VERSION 5
			if (m_user.empty())
			{
				write_uint8(1, p); // 1 authentication method (no auth)
				write_uint8(0, p); // no authentication
			}
			else
			{
				write_uint8(2, p); // 2 authentication methods
				write_uint8(0, p); // no authentication
				write_uint8(2, p); // username/password
			}
			async_write(m_sock, asio::buffer(m_buffer)
				, boost::bind(&socks5_stream::handshake1, this, _1, h));
		}
		else if (m_version == 4)
		{
			socks_connect(h);
		}
		else
		{
			(*h)(error_code(socks_error::unsupported_version, socks_category));
			error_code ec;
			close(ec);
		}
	}

	void socks5_stream::handshake1(error_code const& e, boost::shared_ptr<handler_type> h)
	{
		if (e)
		{
			(*h)(e);
			error_code ec;
			close(ec);
			return;
		}

		m_buffer.resize(2);
		async_read(m_sock, asio::buffer(m_buffer)
			, boost::bind(&socks5_stream::handshake2, this, _1, h));
	}

	void socks5_stream::handshake2(error_code const& e, boost::shared_ptr<handler_type> h)
	{
		if (e)
		{
			(*h)(e);
			error_code ec;
			close(ec);
			return;
		}

		using namespace libtorrent::detail;

		char* p = &m_buffer[0];
		int version = read_uint8(p);
		int method = read_uint8(p);

		if (version < m_version)
		{
			(*h)(error_code(socks_error::unsupported_version, socks_category));
			error_code ec;
			close(ec);
			return;
		}

		if (method == 0)
		{
			socks_connect(h);
		}
		else if (method == 2)
		{
			if (m_user.empty())
			{
				(*h)(error_code(socks_error::username_required, socks_category));
				error_code ec;
				close(ec);
				return;
			}

			// start sub-negotiation
			m_buffer.resize(m_user.size() + m_password.size() + 3);
			char* p = &m_buffer[0];
			write_uint8(1, p);
			write_uint8(m_user.size(), p);
			write_string(m_user, p);
			write_uint8(m_password.size(), p);
			write_string(m_password, p);
			async_write(m_sock, asio::buffer(m_buffer)
				, boost::bind(&socks5_stream::handshake3, this, _1, h));
		}
		else
		{
			(*h)(error_code(socks_error::unsupported_authentication_method, socks_category));
			error_code ec;
			close(ec);
			return;
		}
	}

	void socks5_stream::handshake3(error_code const& e
		, boost::shared_ptr<handler_type> h)
	{
		if (e)
		{
			(*h)(e);
			error_code ec;
			close(ec);
			return;
		}

		m_buffer.resize(2);
		async_read(m_sock, asio::buffer(m_buffer)
			, boost::bind(&socks5_stream::handshake4, this, _1, h));
	}

	void socks5_stream::handshake4(error_code const& e
		, boost::shared_ptr<handler_type> h)
	{
		if (e)
		{
			(*h)(e);
			error_code ec;
			close(ec);
			return;
		}

		using namespace libtorrent::detail;

		char* p = &m_buffer[0];
		int version = read_uint8(p);
		int status = read_uint8(p);

		if (version != 1)
		{
			(*h)(error_code(socks_error::unsupported_authentication_version, socks_category));
			error_code ec;
			close(ec);
			return;
		}

		if (status != 0)
		{
			(*h)(error_code(socks_error::authentication_error, socks_category));
			error_code ec;
			close(ec);
			return;
		}

		std::vector<char>().swap(m_buffer);
		socks_connect(h);
	}

	void socks5_stream::socks_connect(boost::shared_ptr<handler_type> h)
	{
		using namespace libtorrent::detail;

		if (m_version == 5)
		{
			// send SOCKS5 connect command
			m_buffer.resize(6 + (m_remote_endpoint.address().is_v4()?4:16));
			char* p = &m_buffer[0];
			write_uint8(5, p); // SOCKS VERSION 5
			write_uint8(m_command, p); // CONNECT/BIND command
			write_uint8(0, p); // reserved
			write_uint8(m_remote_endpoint.address().is_v4()?1:4, p); // address type
			write_endpoint(m_remote_endpoint, p);
			TORRENT_ASSERT(p - &m_buffer[0] == int(m_buffer.size()));
		}
		else if (m_version == 4)
		{
			m_buffer.resize(m_user.size() + 9);
			char* p = &m_buffer[0];
			write_uint8(4, p); // SOCKS VERSION 4
			write_uint8(m_command, p); // CONNECT/BIND command
			write_uint16(m_remote_endpoint.port(), p);
			write_uint32(m_remote_endpoint.address().to_v4().to_ulong(), p);
			std::copy(m_user.begin(), m_user.end(), p);
			p += m_user.size();
			write_uint8(0, p); // NULL terminator
		}
		else
		{
			(*h)(error_code(socks_error::unsupported_version, socks_category));
			error_code ec;
			close(ec);
			return;
		}

		async_write(m_sock, asio::buffer(m_buffer)
			, boost::bind(&socks5_stream::connect1, this, _1, h));
	}

	void socks5_stream::connect1(error_code const& e, boost::shared_ptr<handler_type> h)
	{
		if (e)
		{
			(*h)(e);
			error_code ec;
			close(ec);
			return;
		}

		if (m_version == 5)
			m_buffer.resize(6 + 4); // assume an IPv4 address
		else if (m_version == 4)
			m_buffer.resize(8);
		async_read(m_sock, asio::buffer(m_buffer)
			, boost::bind(&socks5_stream::connect2, this, _1, h));
	}

	void socks5_stream::connect2(error_code const& e, boost::shared_ptr<handler_type> h)
	{
		if (e)
		{
			(*h)(e);
			error_code ec;
			close(ec);
			return;
		}

		using namespace libtorrent::detail;

		// send SOCKS5 connect command
		char* p = &m_buffer[0];
		int version = read_uint8(p);
		int response = read_uint8(p);

		if (m_version == 5)
		{
			if (version < m_version)
			{
				(*h)(error_code(socks_error::unsupported_version, socks_category));
				error_code ec;
				close(ec);
				return;
			}
			if (response != 0)
			{
				error_code e(socks_error::general_failure, socks_category);
				switch (response)
				{
					case 2: e = asio::error::no_permission; break;
					case 3: e = asio::error::network_unreachable; break;
					case 4: e = asio::error::host_unreachable; break;
					case 5: e = asio::error::connection_refused; break;
					case 6: e = asio::error::timed_out; break;
					case 7: e = error_code(socks_error::command_not_supported, socks_category); break;
					case 8: e = asio::error::address_family_not_supported; break;
				}
				(*h)(e);
				error_code ec;
				close(ec);
				return;
			}
			p += 1; // reserved
			int atyp = read_uint8(p);
			// we ignore the proxy IP it was bound to
			if (atyp == 1)
			{
				if (m_command == 2)
				{
					if (m_listen == 0)
					{
						m_listen = 1;
						connect1(e, h);
						return;
					}
					m_remote_endpoint.address(read_v4_address(p));
					m_remote_endpoint.port(read_uint16(p));
					std::vector<char>().swap(m_buffer);
					(*h)(e);
				}
				else
				{
					std::vector<char>().swap(m_buffer);
					(*h)(e);
				}
				return;
			}
			int extra_bytes = 0;
			if (atyp == 4)
			{
				extra_bytes = 12;
			}
			else if (atyp == 3)
			{
				extra_bytes = read_uint8(p) - 3;
			}
			else
			{
				(*h)(asio::error::address_family_not_supported);
				error_code ec;
				close(ec);
				return;
			}
			m_buffer.resize(m_buffer.size() + extra_bytes);

			TORRENT_ASSERT(extra_bytes > 0);
			async_read(m_sock, asio::buffer(&m_buffer[m_buffer.size() - extra_bytes], extra_bytes)
				, boost::bind(&socks5_stream::connect3, this, _1, h));
		}
		else if (m_version == 4)
		{
			if (version != 0)
			{
				(*h)(error_code(socks_error::general_failure, socks_category));
				error_code ec;
				close(ec);
				return;
			}

			// access granted
			if (response == 90)
			{
				if (m_command == 2)
				{
					if (m_listen == 0)
					{
						m_listen = 1;
						connect1(e, h);
						return;
					}
					m_remote_endpoint.address(read_v4_address(p));
					m_remote_endpoint.port(read_uint16(p));
					std::vector<char>().swap(m_buffer);
					(*h)(e);
				}
				else
				{
					std::vector<char>().swap(m_buffer);
					(*h)(e);
				}
				return;
			}

			int code = socks_error::general_failure;
			switch (response)
			{
				case 91: code = socks_error::authentication_error; break;
				case 92: code = socks_error::no_identd; break;
				case 93: code = socks_error::identd_error; break;
			}
			error_code e(code, socks_category);
			(*h)(e);
			close(e);
		}
	}

	void socks5_stream::connect3(error_code const& e, boost::shared_ptr<handler_type> h)
	{
		using namespace libtorrent::detail;

		if (e)
		{
			(*h)(e);
			error_code ec;
			close(ec);
			return;
		}

		if (m_command == 2)
		{
			if (m_listen == 0)
			{
				m_listen = 1;
				connect1(e, h);
				return;
			}

			char* p = &m_buffer[0];
			p += 2; // version and response code
			int atyp = read_uint8(p);
			TORRENT_ASSERT(atyp == 3 || atyp == 4);
			if (atyp == 4)
			{
				// we don't support resolving the endpoint address
				// if we receive a domain name, just set the remote
				// endpoint to INADDR_ANY
				m_remote_endpoint = tcp::endpoint();
			}
			else if (atyp == 3)
			{
				m_remote_endpoint.address(read_v4_address(p));
				m_remote_endpoint.port(read_uint16(p));
			}
		}
		std::vector<char>().swap(m_buffer);
		(*h)(e);
	}
}

