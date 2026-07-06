#pragma once

#include <boost/system/result.hpp>
#include <boost/url/pct_string_view.hpp>
#include <boost/url/grammar/delim_rule.hpp>
#include <boost/url/grammar/optional_rule.hpp>
#include <boost/url/grammar/range_rule.hpp>
#include <boost/url/grammar/tuple_rule.hpp>


namespace AqualinkAutomate::HTTP::Routing
{

    class segment_template
    {
        enum class modifier : unsigned char
        {
            none, 
            optional,   // {id?}
            star,       // {id*}
            plus        // {id+}
        };

        std::string str_;
        bool is_literal_ = true;
        modifier modifier_ = modifier::none;

        friend struct segment_template_rule_t;

    public:
        constexpr segment_template() = default;

        bool match(boost::urls::pct_string_view seg) const;

        std::string_view string() const
        {
            return str_;
        }

        std::string_view id() const;

        bool empty() const
        {
            return str_.empty();
        }

        bool is_literal() const
        {
            return is_literal_;
        }

        constexpr bool has_modifier() const
        {
            return !is_literal() && modifier_ != modifier::none;
        }

        bool is_optional() const
        {
            return modifier_ == modifier::optional;
        }

        bool is_star() const
        {
            return modifier_ == modifier::star;
        }

        bool is_plus() const
        {
            return modifier_ == modifier::plus;
        }

        friend constexpr bool operator==(segment_template const& a, segment_template const& b)
        {
            if (a.is_literal_ != b.is_literal_)
            {
                return false;
            }

            if (a.is_literal_)
            {
                return a.str_ == b.str_;
            }

            return a.modifier_ == b.modifier_;
        }

        // segments have precedence:
        //     - literal
        //     - unique
        //     - optional
        //     - plus
        //     - star

        friend constexpr bool operator<(segment_template const& a, segment_template const& b)
        {
            if (b.is_literal())
            {
                return false;
            }

            if (a.is_literal())
            {
                return !b.is_literal();
            }

            return a.modifier_ < b.modifier_;
        }
    };

    struct segment_template_rule_t
    {
        using value_type = segment_template;

        boost::system::result<value_type> parse(char const*& it, char const* end) const noexcept;
    };

    static constexpr auto segment_template_rule = segment_template_rule_t{};

    static constexpr auto path_template_rule =
        boost::urls::grammar::tuple_rule(
            boost::urls::grammar::squelch(
                boost::urls::grammar::optional_rule(
                    boost::urls::grammar::delim_rule('/'))),
            boost::urls::grammar::range_rule(
                segment_template_rule,
                boost::urls::grammar::tuple_rule(
                    boost::urls::grammar::squelch(boost::urls::grammar::delim_rule('/')),
                    segment_template_rule)));

}
// namespace AqualinkAutomate::HTTP::Routing
