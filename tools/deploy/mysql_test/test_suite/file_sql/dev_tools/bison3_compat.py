#!/usr/bin/env python3
"""Produce a bison-3 compatible copy of the OB grammar.

The OB grammar targets bison 2.4.1 and uses the mid rule references
("$kind_node") that bison 3.x rejects.  This script rewrites those
pre-existing rules so that the grammar can be handed to a modern bison
for a conflict analysis.  The COPY rules are left untouched, only the
pre-existing file_table_expr rules are rewritten.
"""
import re
import sys

src = open(sys.argv[1], encoding='utf-8').read()

# Rewrite the file_table_expr rules: the kind_node is a plain local
# variable, it is not a mid rule reference.
pattern = re.compile(
    r"\{\n"
    r"  ParseNode \*kind_node = NULL;\n"
    r"  malloc_terminal_node\(kind_node, result->malloc_pool_, T_INT\);\n"
    r"  kind_node->value_ = (\d+);\n"
    r"  kind_node->is_hidden_const_ = 1;\n"
    r"  malloc_non_terminal_node\(\$\$, result->malloc_pool_, T_FILE_TABLE_EXPRESSION, 4, kind_node, ([^;]+)\);\n"
    r"\}"
)


def repl(m):
    return ("{\n"
            "  ParseNode *kind_node = NULL;\n"
            "  malloc_terminal_node(kind_node, result->malloc_pool_, T_INT);\n"
            "  kind_node->value_ = %s;\n"
            "  kind_node->is_hidden_const_ = 1;\n"
            "  malloc_non_terminal_node($$, result->malloc_pool_, T_FILE_TABLE_EXPRESSION, 4, kind_node, %s);\n"
            "}" % (m.group(1), m.group(2)))


new, n = pattern.subn(repl, src)
sys.stderr.write("rewrote %d file_table_expr actions\n" % n)
open(sys.argv[2], 'w', encoding='utf-8').write(new)
