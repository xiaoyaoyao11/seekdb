# Produce a bison-3 compatible copy of the OB grammar.
# The OB grammar targets bison 2.4.1 and uses mid rule references
# ("$kind_node") that bison 3.x rejects.  This script rewrites those
# pre-existing rules so that the grammar can be handed to a modern bison
# for a conflict analysis.  The COPY rules are left untouched, only the
# pre-existing file_table_expr rules are rewritten.
param(
  [Parameter(Mandatory = $true)][string]$InFile,
  [Parameter(Mandatory = $true)][string]$OutFile
)

$ErrorActionPreference = 'Stop'
$src = [System.IO.File]::ReadAllText($InFile)

# The mid rule reference "$kind_node" is not known to bison 3.x, the
# variable is a plain local variable of the action.
$pattern = 'malloc_non_terminal_node\(\$\$, result->malloc_pool_, T_FILE_TABLE_EXPRESSION, 4, \$kind_node, '
$count = ([regex]::Matches($src, [regex]::Escape($pattern))).Count
$new = $src.Replace($pattern, 'malloc_non_terminal_node($$, result->malloc_pool_, T_FILE_TABLE_EXPRESSION, 4, kind_node, ')
Write-Output "rewrote $count file_table_expr actions"
[System.IO.File]::WriteAllText($OutFile, $new, (New-Object System.Text.UTF8Encoding($false)))
