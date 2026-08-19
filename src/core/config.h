#pragma once
#include <map>
#include <string>
#include <vector>

// ~/.config/<name>/config (honours XDG_CONFIG_HOME); sectioned ini:
// [section] headers, "key = value", # comments. keys are "section.key" here
std::string config_path();

struct Config {
    std::map<std::string, std::string> v;

    std::string str(const std::string &key) const;
    int num(const std::string &key) const;
    bool flag(const std::string &key) const;
    const std::vector<std::string> &list(const std::string &key) const;  // comma list, lowercased
    void set(const std::string &key, const std::string &val) { v[key] = val; }
};

// persist one key to the config file (in its section, replacing any existing line) and to the
// parsed config; false if the file could not be written
bool config_save(const std::string &key, const std::string &val);

// parsed once; writes a commented default file if none exists
Config &config();

// "25/26" for any instant; the school year rolls on 1 August
std::string school_year(long long t);
// epoch of 1 August of a "25/26" label's first year; 0 if unparsable
long long school_year_start(const std::string &label);
// "25/26 · H1"
std::string period_label(long long t);
// years to scrape, auto-resolved
std::vector<std::string> active_years();
// earliest instant any configured year covers
long long scrape_since();
bool blacklisted(const std::string &klass, const std::string &abbrev);
// user's short name for a raw class name, "" if none ([classes], case-insensitive)
std::string class_override(const std::string &raw);

// whole mark an average rounds to, by the school.avg_round floors
int avg_mark(double avg);
// mark 1..N for a percentage, by the school.points floors
int points_mark(double pct);
// school.mark_scale as digits, e.g. {'1','5'}
std::pair<char, char> mark_scale();

// ini parser + defaults table, exercised by --selfcheck
int config_check();
