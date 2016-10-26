/*
 * Copyright (C) 1996-2016 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

/*
 * Portions of this code are copyrighted and released under GPLv2+ by:
 * Copyright (c) 2011, Marcus Kool
 * Please add new claims to the CONTRIBUTORS file instead.
 */

/* DEBUG: section 28    Access Control */

#include "squid.h"
#include "acl/Acl.h"
#include "acl/Checklist.h"
#include "acl/RegexData.h"
#include "base/RegexPattern.h"
#include "ConfigParser.h"
#include "Debug.h"
#include "sbuf/List.h"

ACLRegexData::~ACLRegexData()
{
}

bool
ACLRegexData::match(char const *word)
{
    if (!word)
        return 0;

    debugs(28, 3, "checking '" << word << "'");

    // walk the list of patterns to see if one matches
    for (auto &i : data) {
        if (i.match(word)) {
            debugs(28, 2, "'" << i.c_str() << "' found in '" << word << "'");
            // TODO: old code also popped the pattern to second place of the list
            // in order to reduce patterns search times.
            return 1;
        }
    }

    return 0;
}

SBufList
ACLRegexData::dump() const
{
    SBufList sl;
    int flags = REG_EXTENDED | REG_NOSUB;

    // walk and dump the list
    // keeping the flags values consistent
    for (auto &i : data) {
        if (i.flags != flags) {
            if ((i.flags&REG_ICASE) != 0) {
                sl.push_back(SBuf("-i"));
            } else {
                sl.push_back(SBuf("+i"));
            }
            flags = i.flags;
        }

        sl.push_back(SBuf(i.c_str()));
    }

    return sl;
}

static const char *
removeUnnecessaryWildcards(char * t)
{
    char * orig = t;

    if (strncmp(t, "^.*", 3) == 0)
        t += 3;

    /* NOTE: an initial '.' might seem unnessary but is not;
     * it can be a valid requirement that cannot be optimised
     */
    while (*t == '.'  &&  *(t+1) == '*') {
        t += 2;
    }

    if (*t == '\0') {
        debugs(28, DBG_IMPORTANT, "" << cfg_filename << " line " << config_lineno << ": " << config_input_line);
        debugs(28, DBG_IMPORTANT, "WARNING: regular expression '" << orig << "' has only wildcards and matches all strings. Using '.*' instead.");
        return ".*";
    }
    if (t != orig) {
        debugs(28, DBG_IMPORTANT, "" << cfg_filename << " line " << config_lineno << ": " << config_input_line);
        debugs(28, DBG_IMPORTANT, "WARNING: regular expression '" << orig << "' has unnecessary wildcard(s). Using '" << t << "' instead.");
    }

    return t;
}

static bool
compileRE(std::list<RegexPattern> &curlist, const char * RE, int flags)
{
    if (RE == NULL || *RE == '\0')
        return curlist.empty(); // XXX: old code did this. It looks wrong.

    regex_t comp;
    if (int errcode = regcomp(&comp, RE, flags)) {
        char errbuf[256];
        regerror(errcode, &comp, errbuf, sizeof errbuf);
        debugs(28, DBG_CRITICAL, "" << cfg_filename << " line " << config_lineno << ": " << config_input_line);
        debugs(28, DBG_CRITICAL, "ERROR: invalid regular expression: '" << RE << "': " << errbuf);
        return false;
    }
    debugs(28, 2, "compiled '" << RE << "' with flags " << flags);

    curlist.emplace_back(flags, RE);
    curlist.back().regex = comp;

    return true;
}

/** Compose and compile one large RE from a set of (small) REs.
 * The ultimate goal is to have only one RE per ACL so that match() is
 * called only once per ACL.
 */
static int
compileOptimisedREs(std::list<RegexPattern> &curlist, const SBufList &sl)
{
    std::list<RegexPattern> newlist;
    int numREs = 0;
    int flags = REG_EXTENDED | REG_NOSUB;
    int largeREindex = 0;
    char largeRE[BUFSIZ];
    *largeRE = 0;

    for (SBuf i : sl) {
        int RElen;
        RElen = i.length();

        static const SBuf minus_i("-i");
        static const SBuf plus_i("+i");
        if (i == minus_i) {
            if (flags & REG_ICASE) {
                /* optimisation of  -i ... -i */
                debugs(28, 2, "compileOptimisedREs: optimisation of -i ... -i" );
            } else {
                debugs(28, 2, "compileOptimisedREs: -i" );
                if (!compileRE(newlist, largeRE, flags))
                    return 0;
                flags |= REG_ICASE;
                largeRE[largeREindex=0] = '\0';
            }
        } else if (i == plus_i) {
            if ((flags & REG_ICASE) == 0) {
                /* optimisation of  +i ... +i */
                debugs(28, 2, "compileOptimisedREs: optimisation of +i ... +i");
            } else {
                debugs(28, 2, "compileOptimisedREs: +i");
                if (!compileRE(newlist, largeRE, flags))
                    return 0;
                flags &= ~REG_ICASE;
                largeRE[largeREindex=0] = '\0';
            }
        } else if (RElen + largeREindex + 3 < BUFSIZ-1) {
            debugs(28, 2, "compileOptimisedREs: adding RE '" << i << "'");
            if (largeREindex > 0) {
                largeRE[largeREindex] = '|';
                ++largeREindex;
            }
            largeRE[largeREindex] = '(';
            ++largeREindex;
            i.copy(largeRE+largeREindex, BUFSIZ-largeREindex);
            largeREindex += i.length();
            largeRE[largeREindex] = ')';
            ++largeREindex;
            largeRE[largeREindex] = '\0';
            ++numREs;
        } else {
            debugs(28, 2, "compileOptimisedREs: buffer full, generating new optimised RE..." );
            if (!compileRE(newlist, largeRE, flags))
                return 0;
            largeRE[largeREindex=0] = '\0';
            continue;    /* do the loop again to add the RE to largeRE */
        }
    }

    if (!compileRE(newlist, largeRE, flags))
        return 0;

    /* all was successful, so put the new list at the tail */
    curlist.splice(curlist.end(), newlist);

    debugs(28, 2, "compileOptimisedREs: " << numREs << " REs are optimised into one RE.");
    if (numREs > 100) {
        debugs(28, (opt_parse_cfg_only?DBG_IMPORTANT:2), "" << cfg_filename << " line " << config_lineno << ": " << config_input_line);
        debugs(28, (opt_parse_cfg_only?DBG_IMPORTANT:2), "WARNING: there are more than 100 regular expressions. " <<
               "Consider using less REs or use rules without expressions like 'dstdomain'.");
    }

    return 1;
}

static void
compileUnoptimisedREs(std::list<RegexPattern> &curlist, const SBufList &sl)
{
    int flags = REG_EXTENDED | REG_NOSUB;

    static const SBuf minus_i("-i"), plus_i("+i");
    for (auto i : sl) {
        if (i == minus_i) {
            flags |= REG_ICASE;
        } else if (i == plus_i) {
            flags &= ~REG_ICASE;
        } else {
            if (!compileRE(curlist, i.c_str() , flags))
                debugs(28, DBG_CRITICAL, "ERROR: Skipping regular expression. Compile failed: '" << i << "'");
        }
    }
}

void
ACLRegexData::parse()
{
    debugs(28, 2, "new Regex line or file");

    SBufList sl;
    while (char *t = ConfigParser::RegexStrtokFile()) {
        const char *clean = removeUnnecessaryWildcards(t);
        if (strlen(clean) > BUFSIZ-1) {
            debugs(28, DBG_CRITICAL, "" << cfg_filename << " line " << config_lineno << ": " << config_input_line);
            debugs(28, DBG_CRITICAL, "ERROR: Skipping regular expression. Larger than " << BUFSIZ-1 << " characters: '" << clean << "'");
        } else {
            debugs(28, 3, "buffering RE '" << clean << "'");
            sl.push_back(SBuf(clean));
        }
    }

    if (!compileOptimisedREs(data, sl)) {
        debugs(28, DBG_IMPORTANT, "WARNING: optimisation of regular expressions failed; using fallback method without optimisation");
        compileUnoptimisedREs(data, sl);
    }
}

bool
ACLRegexData::empty() const
{
    return data.empty();
}

ACLData<char const *> *
ACLRegexData::clone() const
{
    /* Regex's don't clone yet. */
    assert(data.empty());
    return new ACLRegexData;
}

