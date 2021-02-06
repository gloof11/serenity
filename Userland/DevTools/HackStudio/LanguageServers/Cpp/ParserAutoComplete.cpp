/*
 * Copyright (c) 2021, Itamar S. <itamar8910@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "ParserAutoComplete.h"
#include <AK/Assertions.h>
#include <AK/HashTable.h>
#include <LibCpp/AST.h>
#include <LibCpp/Lexer.h>
#include <LibCpp/Parser.h>
#include <LibCpp/Preprocessor.h>
#include <LibRegex/Regex.h>

// #define AUTOCOMPLETE_VERBOSE

#ifdef AUTOCOMPLETE_VERBOSE
#    define VERBOSE(fmt, ...) dbgln(fmt, ##__VA_ARGS__)
#else
#    define VERBOSE(fmt, ...) \
        do {                  \
        } while (0)
#endif

namespace LanguageServers::Cpp {

ParserAutoComplete::ParserAutoComplete(const FileDB& filedb)
    : AutoCompleteEngine(filedb)
{
}

const ParserAutoComplete::DocumentData& ParserAutoComplete::get_or_create_document_data(const String& file)
{
    auto absolute_path = filedb().to_absolute_path(file);
    if (!m_documents.contains(absolute_path)) {
        set_document_data(absolute_path, create_document_data_for(absolute_path));
    }
    return get_document_data(absolute_path);
}

const ParserAutoComplete::DocumentData& ParserAutoComplete::get_document_data(const String& file) const
{
    auto absolute_path = filedb().to_absolute_path(file);
    auto document_data = m_documents.get(absolute_path);
    ASSERT(document_data.has_value());
    return *document_data.value();
}

OwnPtr<ParserAutoComplete::DocumentData> ParserAutoComplete::create_document_data_for(const String& file)
{
    auto document = filedb().get(file);
    ASSERT(document);
    auto content = document->text();
    auto document_data = make<DocumentData>(document->text());
    auto root = document_data->parser.parse();
    for (auto& path : document_data->preprocessor.included_paths()) {
        get_or_create_document_data(document_path_from_include_path(path));
    }
#ifdef DEBUG_AUTOCOMPLETE
    root->dump(0);
#endif
    return move(document_data);
}

void ParserAutoComplete::set_document_data(const String& file, OwnPtr<DocumentData>&& data)
{
    m_documents.set(filedb().to_absolute_path(file), move(data));
}

ParserAutoComplete::DocumentData::DocumentData(String&& _text)
    : text(move(_text))
    , preprocessor(text.view())
    , parser(preprocessor.process().view())
{
}

Vector<GUI::AutocompleteProvider::Entry> ParserAutoComplete::get_suggestions(const String& file, const GUI::TextPosition& autocomplete_position)
{
    ASSERT(autocomplete_position.column() > 0);
    Cpp::Position position { autocomplete_position.line(), autocomplete_position.column() - 1 };

    VERBOSE("ParserAutoComplete position {}:{}", position.line, position.column);

    const auto& document = get_or_create_document_data(file);
    auto node = document.parser.node_at(position);
    if (!node) {
        VERBOSE("no node at position {}:{}", position.line, position.column);
        return {};
    }

    if (!node->is_identifier()) {
        if (is_empty_property(document, *node, position)) {
            ASSERT(node->is_member_expression());
            return autocomplete_property(document, (MemberExpression&)(*node), "");
        }
        return {};
    }

    if (is_property(*node)) {
        return autocomplete_property(document, (MemberExpression&)(*node->parent()), document.parser.text_of_node(*node));
    }

    return autocomplete_identifier(document, *node);
}

Vector<GUI::AutocompleteProvider::Entry> ParserAutoComplete::autocomplete_identifier(const DocumentData& document, const ASTNode& node) const
{
    const Cpp::ASTNode* current = &node;
    NonnullRefPtrVector<Cpp::Declaration> available_declarations;
    while (current) {
        available_declarations.append(current->declarations());
        current = current->parent();
    }
    Vector<StringView> available_names;
    auto add_name = [&available_names](auto& name) {
        if (name.is_null() || name.is_empty())
            return;
        if (!available_names.contains_slow(name))
            available_names.append(name);
    };
    for (auto& decl : available_declarations) {
        if (decl.is_variable_or_parameter_declaration()) {
            add_name(((Cpp::VariableOrParameterDeclaration&)decl).m_name);
        }
    }

    auto partial_text = document.parser.text_of_node(node);
    Vector<GUI::AutocompleteProvider::Entry> suggestions;
    for (auto& name : available_names) {
        if (name.starts_with(partial_text)) {
            suggestions.append({ name.to_string(), partial_text.length(), GUI::AutocompleteProvider::CompletionKind::Identifier });
        }
    }
    return suggestions;
}

Vector<GUI::AutocompleteProvider::Entry> ParserAutoComplete::autocomplete_property(const DocumentData& document, const MemberExpression& parent, const StringView partial_text) const
{
    auto type = type_of(document, *parent.m_object);
    if (type.is_null()) {
        VERBOSE("Could not infer type of object");
        return {};
    }

    Vector<GUI::AutocompleteProvider::Entry> suggestions;
    for (auto& prop : properties_of_type(document, type)) {
        if (prop.name.starts_with(partial_text)) {
            suggestions.append({ prop.name, partial_text.length(), GUI::AutocompleteProvider::CompletionKind::Identifier });
        }
    }
    return suggestions;
}

bool ParserAutoComplete::is_property(const ASTNode& node) const
{
    if (!node.parent()->is_member_expression())
        return false;

    auto& parent = (MemberExpression&)(*node.parent());
    return parent.m_property.ptr() == &node;
}

bool ParserAutoComplete::is_empty_property(const DocumentData& document, const ASTNode& node, const Position& autocomplete_position) const
{
    if (!node.is_member_expression())
        return false;
    auto previous_token = document.parser.token_at(autocomplete_position);
    if (!previous_token.has_value())
        return false;
    return previous_token.value().type() == Token::Type::Dot;
}

String ParserAutoComplete::type_of_property(const DocumentData& document, const Identifier& identifier) const
{
    auto& parent = (const MemberExpression&)(*identifier.parent());
    auto properties = properties_of_type(document, type_of(document, *parent.m_object));
    for (auto& prop : properties) {
        if (prop.name == identifier.m_name)
            return prop.type->m_name;
    }
    return {};
}

String ParserAutoComplete::type_of_variable(const Identifier& identifier) const
{
    const ASTNode* current = &identifier;
    while (current) {
        for (auto& decl : current->declarations()) {
            if (decl.is_variable_or_parameter_declaration()) {
                auto& var_or_param = (VariableOrParameterDeclaration&)decl;
                if (var_or_param.m_name == identifier.m_name) {
                    return var_or_param.m_type->m_name;
                }
            }
        }
        current = current->parent();
    }
    return {};
}

String ParserAutoComplete::type_of(const DocumentData& document, const Expression& expression) const
{
    if (expression.is_member_expression()) {
        auto& member_expression = (const MemberExpression&)expression;
        return type_of_property(document, *member_expression.m_property);
    }
    if (!expression.is_identifier()) {
        ASSERT_NOT_REACHED(); // TODO
    }

    auto& identifier = (const Identifier&)expression;

    if (is_property(identifier))
        return type_of_property(document, identifier);

    return type_of_variable(identifier);
}

Vector<ParserAutoComplete::PropertyInfo> ParserAutoComplete::properties_of_type(const DocumentData& document, const String& type) const
{
    auto declarations = get_declarations_in_outer_scope_including_headers(document);
    Vector<PropertyInfo> properties;
    for (auto& decl : declarations) {
        if (!decl.is_struct_or_class())
            continue;
        auto& struct_or_class = (StructOrClassDeclaration&)decl;
        if (struct_or_class.m_name != type)
            continue;
        for (auto& member : struct_or_class.m_members) {
            properties.append({ member.m_name, member.m_type });
        }
    }
    return properties;
}

NonnullRefPtrVector<Declaration> ParserAutoComplete::get_declarations_in_outer_scope_including_headers(const DocumentData& document) const
{
    NonnullRefPtrVector<Declaration> declarations;
    for (auto& include : document.preprocessor.included_paths()) {
        auto included_document = get_document_data(document_path_from_include_path(include));
        declarations.append(get_declarations_in_outer_scope_including_headers(included_document));
    }
    for (auto& decl : document.parser.root_node()->declarations()) {
        declarations.append(decl);
    }
    return declarations;
}

String ParserAutoComplete::document_path_from_include_path(const StringView& include_path) const
{

    static Regex<PosixExtended> library_include("<(.+)>");
    static Regex<PosixExtended> user_defined_include("\"(.+)\"");

    auto document_path_for_library_include = [&](const StringView& include_path) -> String {
        RegexResult result;
        if (!library_include.search(include_path, result))
            return {};

        auto path = result.capture_group_matches.at(0).at(0).view.u8view();
        return String::formatted("/usr/include/{}", path);
    };

    auto document_path_for_user_defined_include = [&](const StringView& include_path) -> String {
        RegexResult result;
        if (!user_defined_include.search(include_path, result))
            return {};

        return result.capture_group_matches.at(0).at(0).view.u8view();
    };

    auto result = document_path_for_library_include(include_path);
    if (result.is_null())
        result = document_path_for_user_defined_include(include_path);

    return result;
}

void ParserAutoComplete::on_edit(const String& file)
{
    set_document_data(file, create_document_data_for(file));
}

void ParserAutoComplete::file_opened([[maybe_unused]] const String& file)
{
    set_document_data(file, create_document_data_for(file));
}

}
