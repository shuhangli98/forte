/*
 * @BEGIN LICENSE
 *
 * Forte: an open-source plugin to Psi4 (https://github.com/psi4/psi4)
 * that implements a variety of quantum chemistry methods for strongly
 * correlated electrons.
 *
 * Copyright (c) 2012-2020 by its authors (see COPYING, COPYING.LESSER, AUTHORS).
 *
 * The copyrights for code used from other parties are included in
 * the corresponding files.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/.
 *
 * @END LICENSE
 */

#include "psi4/libpsi4util/PsiOutStream.h"
#include "psi4/liboptions/liboptions.h"

#include "base_classes/forte_options.h"
#include "base_classes/mo_space_info.h"
#include "helpers/helpers.h"
#include "helpers/string_algorithms.h"

using namespace pybind11::literals;

using namespace psi;

namespace forte {

py::object process_psi4_array_data(psi::Data& data);

std::string rst_bold(const std::string& s);

std::string option_formatter(const std::string& type, const std::string& label,
                             const std::string& default_value, const std::string& description,
                             const std::string& allowed_values);

ForteOptions::ForteOptions() {}

pybind11::dict ForteOptions::dict() { return dict_; }

py::dict make_dict_entry(const std::string& type, const std::string& group,
                         py::object default_value, const std::string& description) {
    return py::dict("type"_a = type, "group"_a = py::str(group), "value"_a = default_value,
                    "default_value"_a = default_value, "description"_a = description.c_str());
}

py::dict make_dict_entry(const std::string& type, const std::string& group,
                         py::object default_value, py::list allowed_values,
                         const std::string& description) {
    return py::dict("type"_a = type, "group"_a = py::str(group), "value"_a = default_value,
                    "default_value"_a = default_value, "allowed_values"_a = allowed_values,
                    "description"_a = description.c_str());
}

void ForteOptions::set_group(const std::string& group) { group_ = group; }

const std::string& ForteOptions::get_group() { return group_; }

void ForteOptions::add(const std::string& label, const std::string& type, py::object default_value,
                       const std::string& description) {
    std::string label_uc = upper_string(label);
    dict_[label_uc.c_str()] = make_dict_entry(type, group_, default_value, description);
}

void ForteOptions::add(const std::string& label, const std::string& type, py::object default_value,
                       py::list allowed_values, const std::string& description) {
    std::string label_uc = upper_string(label);
    dict_[label_uc.c_str()] =
        make_dict_entry(type, group_, default_value, allowed_values, description);
}

std::pair<py::object, std::string> ForteOptions::get(const std::string& label) {
    std::string label_uc = upper_string(label);
    auto result = std::make_pair(py::cast<py::object>(Py_None), std::string("None"));
    if (dict_.contains(label_uc.c_str())) {
        auto dict_entry = dict_[label_uc.c_str()];
        result = std::make_pair(dict_entry["value"], py::cast<std::string>(dict_entry["type"]));
    } else {
        std::string msg = "Called ForteOptions::get(" + label + ") this option is not registered.";
        throw std::runtime_error(msg);
    }
    return result;
}

void ForteOptions::add_bool(const std::string& label, bool value, const std::string& description) {
    add(label, "bool", py::bool_(value), description);
}

void ForteOptions::add_int(const std::string& label, int value, const std::string& description) {
    add(label, "int", py::int_(value), description);
}

void ForteOptions::add_double(const std::string& label, double value,
                              const std::string& description) {
    add(label, "float", py::float_(value), description);
}

void ForteOptions::add_str(const std::string& label, const std::string& value,
                           const std::string& description) {
    add(label, "str", py::str(value), description);
}

void ForteOptions::add_str(const std::string& label, const std::string& value,
                           const std::vector<std::string>& allowed_values,
                           const std::string& description) {
    auto allowed_values_list = py::list();
    for (const auto& s : allowed_values) {
        allowed_values_list.append(py::str(s));
    }
    add(label, "str", py::str(value), allowed_values_list, description);
}

void ForteOptions::add_array(const std::string& label, const std::string& description) {
    add(label, "gen_list", py::list(), description);
}

void ForteOptions::add_int_array(const std::string& label, const std::string& description) {
    add(label, "int_list", py::list(), description);
}

void ForteOptions::add_double_array(const std::string& label, const std::string& description) {
    add(label, "float_list", py::list(), description);
}

bool ForteOptions::get_bool(const std::string& label) {
    auto value_type = get(label);
    if (value_type.second == "bool") {
        return py::cast<bool>(value_type.first);
    }
    std::string msg = "Called ForteOptions::get_bool(" + label +
                      ") but the type for this option is " + value_type.second;
    throw std::runtime_error(msg);
    return false;
}

int ForteOptions::get_int(const std::string& label) {
    auto value_type = get(label);
    if (value_type.second == "int") {
        return py::cast<int>(value_type.first);
    }
    std::string msg = "Called ForteOptions::get_int(" + label +
                      ") but the type for this option is " + value_type.second;
    throw std::runtime_error(msg);
    return 0;
}

double ForteOptions::get_double(const std::string& label) {
    auto value_type = get(label);
    if (value_type.second == "float") {
        return py::cast<double>(value_type.first);
    }
    std::string msg = "Called ForteOptions::get_double(" + label +
                      ") but the type for this option is " + value_type.second;
    throw std::runtime_error(msg);
    return 0.0;
}

std::string ForteOptions::get_str(const std::string& label) {
    auto value_type = get(label);
    if (value_type.second == "str") {
        return py::cast<std::string>(value_type.first);
    }
    std::string msg = "Called ForteOptions::get_str(" + label +
                      ") but the type for this option is " + value_type.second;
    throw std::runtime_error(msg);
    return std::string();
}

py::list ForteOptions::get_gen_list(const std::string& label) {
    auto value_type = get(label);
    if (value_type.second == "gen_list") {
        return value_type.first;
    }
    std::string msg = "Called ForteOptions::get_gen_list(" + label +
                      ") but the type for this option is " + value_type.second;
    throw std::runtime_error(msg);
    return py::list();
}

std::vector<int> ForteOptions::get_int_vec(const std::string& label) {
    std::vector<int> result;
    auto value_type = get(label);
    if (value_type.second == "int_list") {
        for (const auto& s : value_type.first) {
            result.push_back(py::cast<int>(s));
        }
        return result;
    }
    std::string msg = "Called ForteOptions::get_int_vec(" + label +
                      ") but the type for this option is " + value_type.second;
    throw std::runtime_error(msg);
    return result;
}

std::vector<double> ForteOptions::get_double_vec(const std::string& label) {
    std::vector<double> result;
    auto value_type = get(label);
    if (value_type.second == "float_list") {
        for (const auto& s : value_type.first) {
            result.push_back(py::cast<double>(s));
        }
        return result;
    }
    std::string msg = "Called ForteOptions::get_double_vec(" + label +
                      ") but the type for this option is " + value_type.second;
    throw std::runtime_error(msg);
    return result;
}

void ForteOptions::push_options_to_psi4(psi::Options& options) {
    for (auto item : dict_) {
        auto label = py::cast<std::string>(item.first);
        auto type = py::cast<std::string>(item.second["type"]);
        auto py_default_value = item.second["default_value"];
        if (type == "bool") {
            options.add_bool(label, py::cast<bool>(py_default_value));
        }
        if (type == "int") {
            options.add_int(label, py::cast<int>(py_default_value));
        }
        if (type == "float") {
            options.add_double(label, py::cast<double>(py_default_value));
        }
        if (type == "str") {
            if (item.second.contains("allowed_values")) {
                // Here we take a py list of strings and convert it to a string with spaces
                auto py_allowed_values = item.second["allowed_values"];
                std::vector<std::string> allowed_values_vec;
                for (const auto& s : py_allowed_values) {
                    allowed_values_vec.push_back(py::str(s));
                }
                std::string allowed = to_string(allowed_values_vec, " ");
                options.add_str(label, py::cast<std::string>(py_default_value), allowed);
            } else {
                options.add_str(label, py::cast<std::string>(py_default_value));
            }
        }
        if ((type == "int_list") or (type == "float_list") or (type == "gen_list")) {
            options.add(label, new psi::ArrayType());
        }
    }
}

void ForteOptions::get_options_from_psi4(psi::Options& options) {
    for (auto item : dict_) {
        auto label = py::cast<std::string>(item.first);
        auto type = py::cast<std::string>(item.second["type"]);
        if (type == "bool") {
            bool value = options.get_bool(label);
            item.second["value"] = py::cast(value);
        }
        if (type == "int") {
            int value = options.get_int(label);
            item.second["value"] = py::cast(value);
        }
        if (type == "float") {
            double value = options.get_double(label);
            item.second["value"] = py::cast(value);
        }
        if (type == "str") {
            std::string value = options.get_str(label);
            item.second["value"] = py::cast(value);
        }
        if (type == "int_list") {
            std::vector<int> value = options.get_int_vector(label);
            auto py_list = py::list();
            for (auto e : value) {
                py_list.append(py::int_(e));
            }
            item.second["value"] = py_list;
        }
        if (type == "float_list") {
            std::vector<double> value = options.get_double_vector(label);
            auto py_list = py::list();
            for (auto e : value) {
                py_list.append(py::float_(e));
            }
            item.second["value"] = py_list;
        }
        if (type == "gen_list") {
            auto& psi_array_data = options[label];
            auto py_list = py::list();
            size_t nentry = psi_array_data.size();
            for (size_t i = 0; i < nentry; i++) {
                auto result = process_psi4_array_data(psi_array_data[i]);
                py_list.append(result);
            }
            item.second["value"] = py_list;
        }
    }
}

py::object process_psi4_array_data(psi::Data& data) {
    auto list = py::list();
    if (data.is_array()) {
        outfile->Printf("\n Data is an array -> call again");
        // process each element of the array
        size_t n = data.size();
        for (size_t i = 0; i < n; i++) {
            list.append(process_psi4_array_data(data[i]));
        }
    }
    std::string type = data.type();
    if (type == "int") {
        return py::int_(data.to_integer());
    } else if (type == "double") {
        return py::float_(data.to_double());
    }else if (type == "string") {
        return py::str(data.to_string());
    }
    return std::move(list);
}

std::string ForteOptions::generate_documentation() const {
    std::vector<std::pair<std::string, std::string>> option_docs_list;

    //    for (const auto& opt : bool_opts_) {
    //        const std::string& label = std::get<0>(opt);
    //        const std::string& default_value = std::get<1>(opt) ? "True" : "False";
    //        const std::string& description = std::get<2>(opt);
    //        std::string option_text =
    //            option_formatter("Boolean", label, default_value, description, "");
    //        outfile->Printf("\n %s", label.c_str());
    //        option_docs_list.push_back(std::make_pair(label, option_text));
    //    }

    //    for (const auto& opt : int_opts_) {
    //        const std::string& label = std::get<0>(opt);
    //        const std::string& default_value = std::to_string(std::get<1>(opt));
    //        const std::string& description = std::get<2>(opt);
    //        std::string option_text =
    //            option_formatter("Integer", label, default_value, description, "");
    //        outfile->Printf("\n %s", label.c_str());
    //        option_docs_list.push_back(std::make_pair(label, option_text));
    //    }

    //    for (const auto& opt : double_opts_) {
    //        const std::string& label = std::get<0>(opt);
    //        const std::string& default_value = std::to_string(std::get<1>(opt));
    //        const std::string& description = std::get<2>(opt);
    //        std::string option_text = option_formatter("Double", label, default_value,
    //        description, ""); outfile->Printf("\n %s", label.c_str());
    //        option_docs_list.push_back(std::make_pair(label, option_text));
    //    }

    //    for (const auto& opt : str_opts_) {
    //        const std::string& label = std::get<0>(opt);
    //        const std::string& default_value = std::get<1>(opt);
    //        const std::string& description = std::get<2>(opt);
    //        const std::string& allowed_values = to_string(std::get<3>(opt), ", ");
    //        std::string option_text =
    //            option_formatter("String", label, default_value, description, allowed_values);
    //        outfile->Printf("\n %s", label.c_str());
    //        option_docs_list.push_back(std::make_pair(label, option_text));
    //    }

    //    for (const auto& opt : array_opts_) {
    //        const std::string& label = std::get<0>(opt);
    //        const std::string& description = std::get<1>(opt);
    //        std::string option_text = option_formatter("Array", label, "[]", description, "");
    //        outfile->Printf("\n %s", label.c_str());
    //        option_docs_list.push_back(std::make_pair(label, option_text));
    //    }

    std::sort(option_docs_list.begin(), option_docs_list.end());
    std::vector<std::string> options_lines;

    options_lines.push_back(".. _`sec:options`:\n");
    options_lines.push_back("List of Forte options");
    options_lines.push_back("=====================\n");
    options_lines.push_back(".. sectionauthor:: Francesco A. Evangelista\n");
    for (const auto& p : option_docs_list) {
        options_lines.push_back(p.second);
    }

    return to_string(options_lines, "\n");
}

std::string rst_bold(const std::string& s) { return "**" + s + "**"; }

std::string option_formatter(const std::string& type, const std::string& label,
                             const std::string& default_value, const std::string& description,
                             const std::string& allowed_values) {
    std::string s;

    s += rst_bold(label) + "\n\n";
    s += description + "\n\n";
    s += "* Type: " + type + "\n\n";
    s += "* Default value: " + default_value + "\n\n";
    if (allowed_values.size() > 0) {
        s += "* Allowed values: " + allowed_values;
    }

    return s;
}

} // namespace forte
