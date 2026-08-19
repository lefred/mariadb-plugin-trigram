/* Copyright (c) 2019,2024,2025,2026 MariaDB Corporation
   Copyright (c) 2026 lefred (Frédéric Descamps)

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#define MYSQL_SERVER

#include <my_global.h>
#include <my_sys.h>
#include <sql_class.h>
#include <mysql/plugin_function.h>

#include <algorithm>
#include <exception>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using Character= my_wc_t;

struct Trigram
{
  Character value[3];

  bool operator<(const Trigram &other) const
  {
    for (uint i= 0; i < 3; ++i)
    {
      if (value[i] != other.value[i])
        return value[i] < other.value[i];
    }
    return false;
  }

  bool operator==(const Trigram &other) const
  {
    return value[0] == other.value[0] &&
           value[1] == other.value[1] &&
           value[2] == other.value[2];
  }
};

struct Trigram_hash
{
  size_t operator()(const Trigram &value) const
  {
    size_t hash= 14695981039346656037ULL;
    for (uint i= 0; i < 3; ++i)
    {
      hash^= static_cast<size_t>(value.value[i]);
      hash*= 1099511628211ULL;
    }
    return hash;
  }
};

struct Trigram_data
{
  std::vector<Trigram> ordered;
  std::vector<size_t> word_starts;
  std::vector<size_t> word_ends;
};

static Character next_character(const String &value, const uchar **position)
{
  const uchar *end= reinterpret_cast<const uchar *>(value.end());
  Character character;
  int length= value.charset()->mb_wc(&character, *position, end);
  if (length > 0)
    *position+= length;
  else
    character= *(*position)++;
  return character;
}

static bool is_word_character(const CHARSET_INFO *charset, Character character)
{
  if (character <= 255)
    return my_isalnum(charset, character);
  /* Beyond the Basic Multilingual Plane, my_uni_ctype has no data; treat
     those characters (e.g. emoji) the same way the server's own
     my_mb_ctype_mb() does: as unclassified, i.e. not word characters. */
  if (character > 0xFFFF)
    return false;
  const MY_UNI_CTYPE &page= my_uni_ctype[character >> 8];
  const int ctype= page.ctype ? page.ctype[character & 0xFF] : page.pctype;
  return (ctype & (_MY_U | _MY_L | _MY_NMR)) != 0;
}

static bool build_trigrams(const String &input, Trigram_data *result)
{
  try
  {
    const size_t multiplier= input.charset()->casedn_multiply();
    if (multiplier != 0 &&
        input.length() > static_cast<size_t>(-1) / multiplier)
    {
      my_error(ER_OUTOFMEMORY, MYF(MY_WME), 0);
      return true;
    }
    std::vector<char> folded(input.length() * std::max<size_t>(1, multiplier));
    size_t folded_length= input.charset()->casedn(
        input.ptr(), input.length(), folded.data(), folded.size());
    String normalized(folded.data(), folded_length, input.charset());

    std::vector<Character> word;
    const uchar *position= reinterpret_cast<const uchar *>(normalized.ptr());
    const uchar *end= reinterpret_cast<const uchar *>(normalized.end());

    while (position < end)
    {
      Character character= next_character(normalized, &position);
      if (is_word_character(normalized.charset(), character))
      {
        word.push_back(character);
        continue;
      }

      if (!word.empty())
      {
        std::vector<Character> padded;
        padded.reserve(word.size() + 3);
        padded.push_back(' ');
        padded.push_back(' ');
        padded.insert(padded.end(), word.begin(), word.end());
        padded.push_back(' ');
        result->word_starts.push_back(result->ordered.size());
        for (size_t i= 0; i + 2 < padded.size(); ++i)
          result->ordered.push_back({{padded[i], padded[i + 1],
                                      padded[i + 2]}});
        result->word_ends.push_back(result->ordered.size());
        word.clear();
      }
    }

    if (!word.empty())
    {
      std::vector<Character> padded;
      padded.reserve(word.size() + 3);
      padded.push_back(' ');
      padded.push_back(' ');
      padded.insert(padded.end(), word.begin(), word.end());
      padded.push_back(' ');
      result->word_starts.push_back(result->ordered.size());
      for (size_t i= 0; i + 2 < padded.size(); ++i)
        result->ordered.push_back({{padded[i], padded[i + 1],
                                    padded[i + 2]}});
      result->word_ends.push_back(result->ordered.size());
    }
    return false;
  }
  catch (const std::exception &)
  {
    my_error(ER_OUTOFMEMORY, MYF(MY_WME), 0);
    return true;
  }
}

static std::vector<Trigram> unique_trigrams(
    const std::vector<Trigram> &ordered, size_t first, size_t last)
{
  std::vector<Trigram> result(ordered.begin() + first,
                              ordered.begin() + last);
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

static double similarity(const std::vector<Trigram> &left,
                         const std::vector<Trigram> &right)
{
  size_t common= 0;
  size_t l= 0;
  size_t r= 0;
  while (l < left.size() && r < right.size())
  {
    if (left[l] == right[r])
      ++common, ++l, ++r;
    else if (left[l] < right[r])
      ++l;
    else
      ++r;
  }
  const size_t total= left.size() + right.size() - common;
  return total == 0 ? 1.0 :
         static_cast<double>(common) / static_cast<double>(total);
}

enum Similarity_kind
{
  WHOLE_STRING,
  WORD_EXTENT,
  STRICT_WORD_EXTENT
};

/*
  Find the best similarity between left_set and any contiguous extent of
  "ordered", where an extent is a run of whole "units" (unit_starts holds
  one begin offset per unit plus a trailing sentinel equal to
  ordered.size()). WORD_EXTENT uses one trigram per unit; STRICT_WORD_EXTENT
  uses one word per unit.

  A naive implementation re-derives the unique trigram set of every
  candidate extent from scratch (sort + dedup), which is O(units^2) extents
  times O(n log n) per extent. Instead, for each starting unit we grow the
  window one unit at a time and maintain running counts incrementally, so
  each extension is amortized O(1) per newly-added trigram. That brings the
  total cost down to O(units * n) instead of O(units^2 * n log n).
*/
static double best_extent_similarity(const std::vector<Trigram> &ordered,
                                     const std::vector<size_t> &unit_starts,
                                     const std::vector<Trigram> &left_set)
{
  const std::unordered_set<Trigram, Trigram_hash>
      left_lookup(left_set.begin(), left_set.end());
  const size_t left_size= left_set.size();
  const size_t units= unit_starts.size() - 1;

  double best= 0.0;
  std::unordered_map<Trigram, size_t, Trigram_hash> window_counts;
  for (size_t first= 0; first < units; ++first)
  {
    window_counts.clear();
    size_t distinct= 0;
    size_t common= 0;
    for (size_t last= first; last < units; ++last)
    {
      for (size_t idx= unit_starts[last]; idx < unit_starts[last + 1]; ++idx)
      {
        size_t &count= window_counts[ordered[idx]];
        if (count == 0)
        {
          ++distinct;
          if (left_lookup.count(ordered[idx]))
            ++common;
        }
        ++count;
      }
      const size_t total= left_size + distinct - common;
      const double score= total == 0 ? 1.0 :
          static_cast<double>(common) / static_cast<double>(total);
      if (score > best)
        best= score;
    }
  }
  return best;
}

static bool calculate_similarity(Item **args, Similarity_kind kind,
                                 double *result)
{
  StringBuffer<256> left_buffer;
  StringBuffer<256> right_buffer;
  String *left= args[0]->val_str(&left_buffer);
  if (!left || args[0]->null_value)
    return true;
  String *right= args[1]->val_str(&right_buffer);
  if (!right || args[1]->null_value)
    return true;

  try
  {
    Trigram_data left_data;
    Trigram_data right_data;
    if (build_trigrams(*left, &left_data) ||
        build_trigrams(*right, &right_data))
      return true;
    std::vector<Trigram> left_set=
        unique_trigrams(left_data.ordered, 0, left_data.ordered.size());

    if (kind == WHOLE_STRING)
    {
      *result= similarity(
          left_set, unique_trigrams(right_data.ordered, 0,
                                    right_data.ordered.size()));
      return false;
    }

    if (right_data.ordered.empty())
    {
      *result= left_set.empty() ? 1.0 : 0.0;
      return false;
    }

    if (kind == STRICT_WORD_EXTENT)
    {
      /* Words are contiguous in "ordered" (word_ends[i] == word_starts[i+1]),
         so word_starts plus a trailing sentinel gives valid unit ranges. */
      std::vector<size_t> unit_starts= right_data.word_starts;
      unit_starts.push_back(right_data.word_ends.back());
      *result= best_extent_similarity(right_data.ordered, unit_starts,
                                      left_set);
    }
    else
    {
      std::vector<size_t> unit_starts(right_data.ordered.size() + 1);
      for (size_t i= 0; i < unit_starts.size(); ++i)
        unit_starts[i]= i;
      *result= best_extent_similarity(right_data.ordered, unit_starts,
                                      left_set);
    }
    return false;
  }
  catch (const std::exception &)
  {
    my_error(ER_OUTOFMEMORY, MYF(MY_WME), 0);
    return true;
  }
}

template <typename Item_type, uint Argument_count>
class Create_trigram_function : public Create_native_func
{
public:
  Item *create_native(THD *thd, const LEX_CSTRING *name,
                      List<Item> *item_list) override
  {
    const uint count= item_list ? item_list->elements : 0;
    if (count != Argument_count)
    {
      my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
      return nullptr;
    }
    return new (thd->mem_root) Item_type(thd, *item_list);
  }
};

template <Similarity_kind Kind>
class Item_func_trigram_similarity : public Item_real_func
{
  using Self= Item_func_trigram_similarity<Kind>;

public:
  using Item_real_func::Item_real_func;

  bool fix_length_and_dec(THD *) override
  {
    set_maybe_null();
    decimals= 6;
    max_length= float_length(decimals);
    return false;
  }

  double val_real() override
  {
    double value= 0.0;
    null_value= calculate_similarity(args, Kind, &value);
    return null_value ? 0.0 : value;
  }

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING names[]={
      "trigram_similarity"_LEX_CSTRING,
      "trigram_word_similarity"_LEX_CSTRING,
      "trigram_strict_word_similarity"_LEX_CSTRING
    };
    return names[Kind];
  }

  Item *shallow_copy(THD *thd) const override
  {
    return get_item_copy<Self>(thd, this);
  }

  static Plugin_function *plugin_descriptor()
  {
    static Create_trigram_function<Self, 2> creator;
    static Plugin_function descriptor(&creator);
    return &descriptor;
  }
};

using Item_func_similarity= Item_func_trigram_similarity<WHOLE_STRING>;
using Item_func_word_similarity= Item_func_trigram_similarity<WORD_EXTENT>;
using Item_func_strict_word_similarity=
    Item_func_trigram_similarity<STRICT_WORD_EXTENT>;

class Item_func_trigram_distance : public Item_real_func
{
  using Self= Item_func_trigram_distance;
public:
  using Item_real_func::Item_real_func;
  bool fix_length_and_dec(THD *) override
  {
    set_maybe_null();
    decimals= 6;
    max_length= float_length(decimals);
    return false;
  }
  double val_real() override
  {
    double value= 0.0;
    null_value= calculate_similarity(args, WHOLE_STRING, &value);
    return null_value ? 0.0 : 1.0 - value;
  }
  LEX_CSTRING func_name_cstring() const override
  { static LEX_CSTRING name= "trigram_distance"_LEX_CSTRING; return name; }
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Self>(thd, this); }
  static Plugin_function *plugin_descriptor()
  {
    static Create_trigram_function<Self, 2> creator;
    static Plugin_function descriptor(&creator);
    return &descriptor;
  }
};

class Item_func_trigram_count : public Item_longlong_func
{
  using Self= Item_func_trigram_count;
public:
  using Item_longlong_func::Item_longlong_func;
  bool fix_length_and_dec(THD *thd) override
  {
    set_maybe_null();
    unsigned_flag= true;
    return Item_longlong_func::fix_length_and_dec(thd);
  }
  longlong val_int() override
  {
    StringBuffer<256> buffer;
    String *value= args[0]->val_str(&buffer);
    if (!value || args[0]->null_value)
      return null_value= true, 0;
    Trigram_data data;
    if (build_trigrams(*value, &data))
      return null_value= true, 0;
    try
    {
      null_value= false;
      return static_cast<longlong>(
          unique_trigrams(data.ordered, 0, data.ordered.size()).size());
    }
    catch (const std::exception &)
    {
      my_error(ER_OUTOFMEMORY, MYF(MY_WME), 0);
      return null_value= true, 0;
    }
  }
  LEX_CSTRING func_name_cstring() const override
  { static LEX_CSTRING name= "trigram_count"_LEX_CSTRING; return name; }
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Self>(thd, this); }
  static Plugin_function *plugin_descriptor()
  {
    static Create_trigram_function<Self, 1> creator;
    static Plugin_function descriptor(&creator);
    return &descriptor;
  }
};

class Item_func_trigrams : public Item_str_func
{
  using Self= Item_func_trigrams;
public:
  using Item_str_func::Item_str_func;
  bool fix_length_and_dec(THD *) override
  {
    set_maybe_null();
    max_length= MAX_BLOB_WIDTH;
    return agg_arg_charsets_for_string_result(collation, args, 1);
  }
  String *val_str(String *output) override
  {
    StringBuffer<256> buffer;
    String *value= args[0]->val_str(&buffer);
    if (!value || args[0]->null_value)
      return null_value= true, nullptr;
    Trigram_data data;
    if (build_trigrams(*value, &data))
      return null_value= true, nullptr;
    try
    {
      std::vector<Trigram> values=
          unique_trigrams(data.ordered, 0, data.ordered.size());
      output->length(0);
      output->set_charset(value->charset());
      if (output->append('['))
        return null_value= true, nullptr;
      for (size_t i= 0; i < values.size(); ++i)
      {
        if ((i && output->append(',')) || output->append('"'))
          return null_value= true, nullptr;
        for (uint j= 0; j < 3; ++j)
        {
          Character character= values[i].value[j];
          if ((character == '"' || character == '\\') &&
              output->append('\\'))
            return null_value= true, nullptr;
          if (character <= 0x7f)
          {
            if (output->append(static_cast<char>(character)))
              return null_value= true, nullptr;
          }
          else
          {
            char escaped[13];
            int length;
            if (character <= 0xffff)
              length= my_snprintf(escaped, sizeof(escaped), "\\u%04x",
                                  static_cast<uint>(character));
            else
            {
              Character adjusted= character - 0x10000;
              length= my_snprintf(
                  escaped, sizeof(escaped), "\\u%04x\\u%04x",
                  static_cast<uint>(0xd800 + (adjusted >> 10)),
                  static_cast<uint>(0xdc00 + (adjusted & 0x3ff)));
            }
            if (length < 0 ||
                output->append(escaped, static_cast<size_t>(length)))
              return null_value= true, nullptr;
          }
        }
        if (output->append('"'))
          return null_value= true, nullptr;
      }
      if (output->append(']'))
        return null_value= true, nullptr;
      null_value= false;
      return output;
    }
    catch (const std::exception &)
    {
      my_error(ER_OUTOFMEMORY, MYF(MY_WME), 0);
      return null_value= true, nullptr;
    }
  }
  LEX_CSTRING func_name_cstring() const override
  { static LEX_CSTRING name= "trigrams"_LEX_CSTRING; return name; }
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Self>(thd, this); }
  static Plugin_function *plugin_descriptor()
  {
    static Create_trigram_function<Self, 1> creator;
    static Plugin_function descriptor(&creator);
    return &descriptor;
  }
};

class Item_func_trigram_match : public Item_bool_func
{
  using Self= Item_func_trigram_match;
public:
  using Item_bool_func::Item_bool_func;
  bool fix_length_and_dec(THD *thd) override
  {
    set_maybe_null();
    return Item_bool_func::fix_length_and_dec(thd);
  }
  bool val_bool() override
  {
    double threshold= args[2]->val_real();
    if (args[2]->null_value || threshold < 0.0 || threshold > 1.0)
      return null_value= true, false;
    double value= 0.0;
    null_value= calculate_similarity(args, WHOLE_STRING, &value);
    return !null_value && value >= threshold;
  }
  LEX_CSTRING func_name_cstring() const override
  { static LEX_CSTRING name= "trigram_match"_LEX_CSTRING; return name; }
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Self>(thd, this); }
  static Plugin_function *plugin_descriptor()
  {
    static Create_trigram_function<Self, 3> creator;
    static Plugin_function descriptor(&creator);
    return &descriptor;
  }
};

} // namespace

#define TRIGRAM_PLUGIN(item, sql_name, description)                         \
  { MariaDB_FUNCTION_PLUGIN, item::plugin_descriptor(), sql_name, "lefred", \
    description, PLUGIN_LICENSE_GPL, nullptr, nullptr, 0x0100, nullptr,      \
    nullptr, "0.2.0", MariaDB_PLUGIN_MATURITY_BETA }

maria_declare_plugin(trigram)
  TRIGRAM_PLUGIN(Item_func_similarity, "trigram_similarity",
                 "Function TRIGRAM_SIMILARITY()"),
  TRIGRAM_PLUGIN(Item_func_word_similarity, "trigram_word_similarity",
                 "Function TRIGRAM_WORD_SIMILARITY()"),
  TRIGRAM_PLUGIN(Item_func_strict_word_similarity,
                 "trigram_strict_word_similarity",
                 "Function TRIGRAM_STRICT_WORD_SIMILARITY()"),
  TRIGRAM_PLUGIN(Item_func_strict_word_similarity,
                 "trigram_strict_world_similariry",
                 "Compatibility alias for TRIGRAM_STRICT_WORD_SIMILARITY()"),
  TRIGRAM_PLUGIN(Item_func_trigrams, "trigrams", "Function TRIGRAMS()"),
  TRIGRAM_PLUGIN(Item_func_trigram_count, "trigram_count",
                 "Function TRIGRAM_COUNT()"),
  TRIGRAM_PLUGIN(Item_func_trigram_distance, "trigram_distance",
                 "Function TRIGRAM_DISTANCE()"),
  TRIGRAM_PLUGIN(Item_func_trigram_match, "trigram_match",
                 "Function TRIGRAM_MATCH()")
maria_declare_plugin_end;
