// Copyright (c) 2014-2020, The Monero Project
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "common/dns_utils.h"
// check local first (in the event of static or in-source compilation of libunbound)
#include "unbound.h"

#include <stdlib.h>
#include "include_base_utils.h"
#include "common/threadpool.h"
#include "crypto/crypto.h"
#include <boost/thread/mutex.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/optional.hpp>
using namespace epee;

#undef MONERO_DEFAULT_LOG_CATEGORY
#define MONERO_DEFAULT_LOG_CATEGORY "net.dns"

static const char *DEFAULT_DNS_PUBLIC_ADDR[] = {
    "194.150.168.168", // CCC (Germany)
    "80.67.169.40",    // FDN (France)
    "89.233.43.71",    // http://censurfridns.dk (Denmark)
    "109.69.8.51",     // punCAT (Spain)
    "193.58.251.251",  // SkyDNS (Russia)
};

static boost::mutex instance_lock;

namespace
{
    /** return the built in root DS trust anchor */
    static const char *const *
    get_builtin_ds(void)
    {
        static const char *const ds[] =
            {
                ". IN DS 20326 8 2 E06D44B80B8F1D39A95C0B0D7C65D08458E880409BBC683457104237C7F8EC8D\n",
                NULL};
        return ds;
    }

} // anonymous namespace

namespace tools
{

    static const char *get_record_name(int record_type)
    {
        switch (record_type)
        {
        case DNS_TYPE_A:
            return "A";
        case DNS_TYPE_TXT:
            return "TXT";
        case DNS_TYPE_AAAA:
            return "AAAA";
        default:
            return "unknown";
        }
    }

    boost::optional<std::string> ipv4_to_string(const char *src, size_t len)
    {
        if (len < 4)
        {
            MERROR("Invalid IPv4 address: " << std::string(src, len));
            return boost::none;
        }

        std::stringstream ss;
        unsigned int bytes[4];
        for (int i = 0; i < 4; i++)
        {
            unsigned char a = src[i];
            bytes[i] = a;
        }
        ss << bytes[0] << "."
           << bytes[1] << "."
           << bytes[2] << "."
           << bytes[3];
        return ss.str();
    }

    // this obviously will need to change, but is here to reflect the above
    // stop-gap measure and to make the tests pass at least...
    boost::optional<std::string> ipv6_to_string(const char *src, size_t len)
    {
        if (len < 8)
        {
            MERROR("Invalid IPv4 address: " << std::string(src, len));
            return boost::none;
        }

        std::stringstream ss;
        unsigned int bytes[8];
        for (int i = 0; i < 8; i++)
        {
            unsigned char a = src[i];
            bytes[i] = a;
        }
        ss << bytes[0] << ":"
           << bytes[1] << ":"
           << bytes[2] << ":"
           << bytes[3] << ":"
           << bytes[4] << ":"
           << bytes[5] << ":"
           << bytes[6] << ":"
           << bytes[7];
        return ss.str();
    }

    boost::optional<std::string> txt_to_string(const char *src, size_t len)
    {
        if (len == 0)
            return boost::none;
        return std::string(src + 1, len - 1);
    }

    // custom smart pointer.
    // TODO: see if std::auto_ptr and the like support custom destructors
    template <typename type, void (*freefunc)(type *)>
    class scoped_ptr
    {
    public:
        scoped_ptr() : ptr(nullptr)
        {
        }
        scoped_ptr(type *p) : ptr(p)
        {
        }
        ~scoped_ptr()
        {
            freefunc(ptr);
        }
        operator type *() { return ptr; }
        type **operator&() { return &ptr; }
        type *operator->() { return ptr; }
        operator const type *() const { return &ptr; }

    private:
        type *ptr;
    };

    typedef class scoped_ptr<ub_result, ub_resolve_free> ub_result_ptr;

    struct DNSResolverData
    {
        ub_ctx *m_ub_context;
    };

    // work around for bug https://www.nlnetlabs.nl/bugs-script/show_bug.cgi?id=515 needed for it to compile on e.g. Debian 7
    class string_copy
    {
    public:
        string_copy(const char *s) : str(strdup(s)) {}
        ~string_copy() { free(str); }
        operator char *() { return str; }

    public:
        char *str;
    };

    static void add_anchors(ub_ctx *ctx)
    {
        const char *const *ds = ::get_builtin_ds();
        while (*ds)
        {
            MINFO("adding trust anchor: " << *ds);
            ub_ctx_add_ta(ctx, string_copy(*ds++));
        }
    }

    DNSResolver::DNSResolver() : m_data(new DNSResolverData())
    {
        std::vector<std::string> dns_public_addr;
        const char *DNS_PUBLIC = getenv("DNS_PUBLIC");
        if (DNS_PUBLIC)
        {
            dns_public_addr = tools::dns_utils::parse_dns_public(DNS_PUBLIC);
            if (dns_public_addr.empty())
                MERROR("Failed to parse DNS_PUBLIC");
        }

        // init libunbound context
        m_data->m_ub_context = ub_ctx_create();

        for (const auto &ip : dns_public_addr)
            ub_ctx_set_fwd(m_data->m_ub_context, string_copy(ip.c_str()));

        ub_ctx_set_option(m_data->m_ub_context, string_copy("do-udp:"), string_copy("no"));
        ub_ctx_set_option(m_data->m_ub_context, string_copy("do-tcp:"), string_copy("yes"));

        add_anchors(m_data->m_ub_context);
    }

    DNSResolver::~DNSResolver()
    {
        if (m_data)
        {
            if (m_data->m_ub_context != NULL)
            {
                ub_ctx_delete(m_data->m_ub_context);
            }
            delete m_data;
        }
    }

    std::vector<std::string> DNSResolver::get_record(const std::string &url, int record_type, boost::optional<std::string> (*reader)(const char *, size_t), bool &dnssec_available, bool &dnssec_valid)
    {
        std::vector<std::string> addresses;
        dnssec_available = false;
        dnssec_valid = false;

        if (!check_address_syntax(url.c_str()))
        {
            return addresses;
        }

        // destructor takes care of cleanup
        ub_result_ptr result;

        // call DNS resolver, blocking.  if return value not zero, something went wrong
        if (!ub_resolve(m_data->m_ub_context, string_copy(url.c_str()), record_type, DNS_CLASS_IN, &result))
        {
            dnssec_available = (result->secure || result->bogus);
            dnssec_valid = result->secure && !result->bogus;
            if (result->havedata)
            {
                for (size_t i = 0; result->data[i] != NULL; i++)
                {
                    boost::optional<std::string> res = (*reader)(result->data[i], result->len[i]);
                    if (res)
                    {
                        MINFO("Found \"" << *res << "\" in " << get_record_name(record_type) << " record for " << url);
                        addresses.push_back(*res);
                    }
                }
            }
        }

        return addresses;
    }

    std::vector<std::string> DNSResolver::get_ipv4(const std::string &url, bool &dnssec_available, bool &dnssec_valid)
    {
        return get_record(url, DNS_TYPE_A, ipv4_to_string, dnssec_available, dnssec_valid);
    }

    std::vector<std::string> DNSResolver::get_ipv6(const std::string &url, bool &dnssec_available, bool &dnssec_valid)
    {
        return get_record(url, DNS_TYPE_AAAA, ipv6_to_string, dnssec_available, dnssec_valid);
    }

    std::vector<std::string> DNSResolver::get_txt_record(const std::string &url, bool &dnssec_available, bool &dnssec_valid)
    {
        return get_record(url, DNS_TYPE_TXT, txt_to_string, dnssec_available, dnssec_valid);
    }

    std::string DNSResolver::get_dns_format_from_oa_address(const std::string &oa_addr)
    {
        std::string addr(oa_addr);
        auto first_at = addr.find("@");
        if (first_at == std::string::npos)
            return addr;

        // convert name@domain.tld to name.domain.tld
        addr.replace(first_at, 1, ".");

        return addr;
    }

    DNSResolver &DNSResolver::instance()
    {
        boost::lock_guard<boost::mutex> lock(instance_lock);

        static DNSResolver staticInstance;
        return staticInstance;
    }

    DNSResolver DNSResolver::create()
    {
        return DNSResolver();
    }

    bool DNSResolver::check_address_syntax(const char *addr) const
    {
        // if string doesn't contain a dot, we won't consider it a url for now.
        if (strchr(addr, '.') == NULL)
        {
            return false;
        }
        return true;
    }

    namespace dns_utils
    {

        //-----------------------------------------------------------------------
        // TODO: parse the string in a less stupid way, probably with regex
        std::string address_from_txt_record(const std::string &s)
        {
            // make sure the txt record has "oa1:wazn" and find it
            auto pos = s.find("oa1:xmr");
            if (pos == std::string::npos)
                return {};
            // search from there to find "recipient_address="
            pos = s.find("recipient_address=", pos);
            if (pos == std::string::npos)
                return {};
            pos += 18; // move past "recipient_address="
            // find the next semicolon
            auto pos2 = s.find(";", pos);
            if (pos2 != std::string::npos)
            {
                // length of address == 95, we can at least validate that much here
                if (pos2 - pos == 95)
                {
                    return s.substr(pos, 95);
                }
                else if (pos2 - pos == 106) // length of address == 106 --> integrated address
                {
                    return s.substr(pos, 106);
                }
            }
            return {};
        }

        std::vector<std::string> addresses_from_url(const std::string &url, bool &dnssec_valid)
        {
            std::vector<std::string> addresses;
            // get txt records
            bool dnssec_available, dnssec_isvalid;
            std::string oa_addr = DNSResolver::instance().get_dns_format_from_oa_address(url);
            auto records = DNSResolver::instance().get_txt_record(oa_addr, dnssec_available, dnssec_isvalid);

            // TODO: update this to allow for conveying that dnssec was not available
            if (dnssec_available && dnssec_isvalid)
            {
                dnssec_valid = true;
            }
            else
                dnssec_valid = false;

            // for each txt record, try to find a WAZN address in it.
            for (auto &rec : records)
            {
                std::string addr = address_from_txt_record(rec);
                if (addr.size())
                {
                    addresses.push_back(addr);
                }
            }
            return addresses;
        }

        std::string get_account_address_as_str_from_url(const std::string &url, bool &dnssec_valid, std::function<std::string(const std::string &, const std::vector<std::string> &, bool)> dns_confirm)
        {
            // attempt to get address from dns query
            auto addresses = addresses_from_url(url, dnssec_valid);
            if (addresses.empty())
            {
                LOG_ERROR("wrong address: " << url);
                return {};
            }
            return dns_confirm(url, addresses, dnssec_valid);
        }

        namespace
        {
            bool dns_records_match(const std::vector<std::string> &a, const std::vector<std::string> &b)
            {
                if (a.size() != b.size())
                    return false;

                for (const auto &record_in_a : a)
                {
                    bool ok = false;
                    for (const auto &record_in_b : b)
                    {
                        if (record_in_a == record_in_b)
                        {
                            ok = true;
                            break;
                        }
                    }
                    if (!ok)
                        return false;
                }

                return true;
            }
        } // namespace

        bool load_txt_records_from_dns(std::vector<std::string> &good_records, const std::vector<std::string> &dns_urls)
        {
            // Prevent infinite recursion when distributing
            if (dns_urls.empty())
                return false;

            std::vector<std::vector<std::string>> records;
            records.resize(dns_urls.size());

            size_t first_index = crypto::rand_idx(dns_urls.size());

            // send all requests in parallel
            std::deque<bool> avail(dns_urls.size(), false), valid(dns_urls.size(), false);
            tools::threadpool &tpool = tools::threadpool::getInstance();
            tools::threadpool::waiter waiter(tpool);
            for (size_t n = 0; n < dns_urls.size(); ++n)
            {
                tpool.submit(&waiter, [n, dns_urls, &records, &avail, &valid]() {
                    records[n] = tools::DNSResolver::instance().get_txt_record(dns_urls[n], avail[n], valid[n]);
                });
            }
            waiter.wait();

            size_t cur_index = first_index;
            do
            {
                const std::string &url = dns_urls[cur_index];
                if (!avail[cur_index])
                {
                    records[cur_index].clear();
                    LOG_PRINT_L2("DNSSEC not available for hostname: " << url << ", skipping.");
                }
                if (!valid[cur_index])
                {
                    records[cur_index].clear();
                    LOG_PRINT_L2("DNSSEC validation failed for hostname: " << url << ", skipping.");
                }

                cur_index++;
                if (cur_index == dns_urls.size())
                {
                    cur_index = 0;
                }
            } while (cur_index != first_index);

            size_t num_valid_records = 0;

            for (const auto &record_set : records)
            {
                if (record_set.size() != 0)
                {
                    num_valid_records++;
                }
            }

            if (num_valid_records == 0)
            {
                LOG_PRINT_L1("Unable to find valid DNS record");
                return false;
            }

            //WAZN has currently only one dns update url. So if we have made it this far
            //we have a dnssec verified update record, so accept it. it is after all only for notification purposes
            //the code will automatically require 2 if it comes a time when we can add a second domain
            if (dns_urls.size() == 1)
            {
                good_records = records[0];
                return true;
            }
            int good_records_index = -1;
            for (size_t i = 0; i < records.size() - 1; ++i)
            {
                if (records[i].size() == 0)
                    continue;

                for (size_t j = i + 1; j < records.size(); ++j)
                {
                    if (dns_records_match(records[i], records[j]))
                    {
                        good_records_index = i;
                        break;
                    }
                }
                if (good_records_index >= 0)
                    break;
            }

            if (good_records_index < 0)
            {
                LOG_PRINT_L0("WARNING: no two DNS TXT records matched");
                return false;
            }

            good_records = records[good_records_index];
            return true;
        }

        std::vector<std::string> parse_dns_public(const char *s)
        {
            unsigned ip0, ip1, ip2, ip3;
            char c;
            std::vector<std::string> dns_public_addr;
            if (!strcmp(s, "tcp"))
            {
                for (size_t i = 0; i < sizeof(DEFAULT_DNS_PUBLIC_ADDR) / sizeof(DEFAULT_DNS_PUBLIC_ADDR[0]); ++i)
                    dns_public_addr.push_back(DEFAULT_DNS_PUBLIC_ADDR[i]);
                LOG_PRINT_L0("Using default public DNS server(s): " << boost::join(dns_public_addr, ", ") << " (TCP)");
            }
            else if (sscanf(s, "tcp://%u.%u.%u.%u%c", &ip0, &ip1, &ip2, &ip3, &c) == 4)
            {
                if (ip0 > 255 || ip1 > 255 || ip2 > 255 || ip3 > 255)
                {
                    MERROR("Invalid IP: " << s << ", using default");
                }
                else
                {
                    dns_public_addr.push_back(std::string(s + strlen("tcp://")));
                }
            }
            else
            {
                MERROR("Invalid DNS_PUBLIC contents, ignored");
            }
            return dns_public_addr;
        }

    } // namespace dns_utils

} // namespace tools
