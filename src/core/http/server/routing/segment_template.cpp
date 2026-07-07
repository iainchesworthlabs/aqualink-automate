#include <boost/url/decode_view.hpp>
#include <boost/url/detail/replacement_field_rule.hpp>
#include <boost/url/grammar/alnum_chars.hpp>
#include <boost/url/grammar/alpha_chars.hpp>
#include <boost/url/grammar/lut_chars.hpp>
#include <boost/url/grammar/token_rule.hpp>
#include <boost/url/grammar/variant_rule.hpp>
#include <boost/url/rfc/detail/path_rules.hpp>
#include "http/server/routing/segment_template.h"

namespace AqualinkAutomate::HTTP::Routing
{

    bool segment_template::match(boost::urls::pct_string_view seg) const
    {
        if (is_literal_)
        {
            return *seg == str_;
        }

        // other nodes match any string
        return true;
    }

    std::string_view segment_template::id() const
    {        
        BOOST_ASSERT(!is_literal());
        
        std::string_view r = { str_ };
        r.remove_prefix(1);
        r.remove_suffix(1);
 
        if (r.ends_with('?') || r.ends_with('+') || r.ends_with('*')) 
        {
            r.remove_suffix(1);
        }

        return r;
    }

    auto segment_template_rule_t::parse(char const*& it,char const* end) const noexcept -> boost::system::result<value_type>
    {
        segment_template t;
        if (it != end && *it == '{')
        {
            auto it0 = it;
            
            ++it;
            
            if (auto send = boost::urls::grammar::find_if(it, end, boost::urls::grammar::lut_chars('}')); send != end)
            {
                std::string_view s(it, send);
                
                static constexpr auto modifiers_cs = boost::urls::grammar::lut_chars("?*+");
                static constexpr auto id_rule = boost::urls::grammar::tuple_rule(boost::urls::grammar::optional_rule(boost::urls::detail::arg_id_rule), boost::urls::grammar::optional_rule(boost::urls::grammar::delim_rule(modifiers_cs)));
                
                if (s.empty() || boost::urls::grammar::parse(s, id_rule))
                {
                    it = send + 1;

                    t.str_ = std::string_view(it0, send + 1);
                    t.is_literal_ = false;

                    if (s.ends_with('?')) 
                    {
                        t.modifier_ = segment_template::modifier::optional;
                    }
                    else if (s.ends_with('*'))
                    {
                        t.modifier_ = segment_template::modifier::star;
                    }
                    else if (s.ends_with('+')) 
                    {
                        t.modifier_ = segment_template::modifier::plus;
                    }

                    return t;
                }
            }
            it = it0;
        }

        auto rv = boost::urls::grammar::parse(it, end, boost::urls::detail::segment_rule);
        BOOST_ASSERT(rv);
        rv->decode({}, boost::urls::string_token::assign_to(t.str_));
        
        t.is_literal_ = true;

        return t;
    }

}
// namespace AqualinkAutomate::HTTP::Routing
