if exists("b:current_syntax")
  finish
endif

syntax case match

" Core language keywords
syntax keyword zapConditional if else iftype match
syntax keyword zapRepeat while for
syntax keyword zapStatement return break continue import as unsafe new self where weak
syntax keyword zapStorageClass fun ext pub priv prot global const var ref module impl static
syntax match zapFunHeader "\<fun\>\s\+\zs[A-Za-z_][A-Za-z0-9_]*\>"
syntax match zapUnsafeFunHeader "\<unsafe\>\s\+\<fun\>\s\+\zs[A-Za-z_][A-Za-z0-9_]*\>"
syntax match zapExtFunHeader "\<ext\>\s\+\<fun\>\s\+\zs[A-Za-z_][A-Za-z0-9_]*\>"
syntax keyword zapStructure record struct class enum alias

" Literals and comments
syntax region zapString start=+"+ skip=+\\.+ end=+"+ contains=zapEscape
syntax match zapEscape +\\["\\abfnrtv0]+ contained
syntax match zapEscape +\\x[0-9a-fA-F]\{2}+ contained
syntax match zapEscape +\\u[0-9a-fA-F]\{4}+ contained
syntax match zapEscape +\\U[0-9a-fA-F]\{8}+ contained
syntax region zapChar start=+'+ skip=+\\.+ end=+'+ contains=zapEscape
syntax match zapNumber "\<[0-9]\+\(\.[0-9]\+\)\?\>"
syntax keyword zapBoolean true false null
syntax match zapLineComment "//.*$" contains=@Spell

" Namespaces and symbols
syntax match zapNamespace "\<[A-Za-z_][A-Za-z0-9_]*\>\ze\s*\."
syntax match zapTypeDecl "\<\%(record\|struct\|unsafe\s\+struct\|class\|enum\|alias\)\s\+\zs[A-Za-z_][A-Za-z0-9_]*\>"
syntax match zapVarDecl "\<\%(var\|const\)\s\+\zs[A-Za-z_][A-Za-z0-9_]*\>"
syntax match zapParameter "\<\%(ref\s\+\)\?\zs[A-Za-z_][A-Za-z0-9_]*\ze\s*:"

" Function names
syntax match zapFunction "\<\%(unsafe\s\+fun\|fun\|ext\s\+fun\)\s\+\zs[A-Za-z_][A-Za-z0-9_]*\>"


" Function calls (exclude declarations like `fun name(` / `unsafe fun name(` / `ext fun name(`)
syntax match zapFunctionCall "\%(\<fun\>\s\+\)\@<!\%(\<unsafe\>\s\+\<fun\>\s\+\)\@<!\%(\<ext\>\s\+\<fun\>\s\+\)\@<!\<[A-Za-z_][A-Za-z0-9_]*\>\ze\s*("

" Generic declarations after function names: fun name<T, U>(...)
" This is explicitly scoped so it does not interfere with non-generic code.
syntax region zapFunctionGenericDecl
      \ start="\%(\<\%(unsafe\s\+fun\|fun\|ext\s\+fun\)\s\+[A-Za-z_][A-Za-z0-9_]*\s*\)\@<=<"
      \ end=">"
      \ contains=zapGenericTypeArg,zapGenericComma
      \ keepend

" Generic lists in type positions only (annotations, return types, where/iftype bounds)
syntax region zapGenericDecl
      \ start="<"
      \ end=">"
      \ contained
      \ containedin=zapType,zapReturnType,zapWhereBoundType,zapIftypeMatchType
      \ contains=zapGenericTypeArg,zapGenericComma
      \ keepend

syntax match zapGenericTypeArg "\<[A-Za-z_][A-Za-z0-9_]*\>" contained
syntax match zapGenericComma "," contained

" Types
syntax match zapType ":\s*\zs\%(\.\.\.\s*\)\?\%(\*\s*\)*[A-Za-z_][A-Za-z0-9_]*\%(\s*\.\s*[A-Za-z_][A-Za-z0-9_]*\)*\%(\s*<\s*[A-Za-z_][A-Za-z0-9_]*\%(\s*,\s*[A-Za-z_][A-Za-z0-9_]*\)*\s*>\)\?" contains=zapGenericDecl
syntax match zapReturnType ")\s*\zs\%(\.\.\.\s*\)\?\%(\*\s*\)*[A-Za-z_][A-Za-z0-9_]*\%(\s*\.\s*[A-Za-z_][A-Za-z0-9_]*\)*\%(\s*<\s*[A-Za-z_][A-Za-z0-9_]*\%(\s*,\s*[A-Za-z_][A-Za-z0-9_]*\)*\s*>\)\?" contains=zapGenericDecl

syntax match zapBuiltinType "\<\%(Int\|Int8\|Int16\|Int32\|Int64\|UInt\|UInt8\|UInt16\|UInt32\|UInt64\|Float\|Float32\|Float64\|Bool\|Void\|Char\|String\)\>"

" where / iftype specifics
syntax match zapWhereTypeParam "\<where\>\s\+\zs[A-Za-z_][A-Za-z0-9_]*\ze\s*:"
syntax match zapWhereBoundType "\<where\>\s\+[A-Za-z_][A-Za-z0-9_]*\s*:\s*\zs[A-Za-z_][A-Za-z0-9_]*\%(\s*\.\s*[A-Za-z_][A-Za-z0-9_]*\)*\%(\s*<\s*[A-Za-z_][A-Za-z0-9_]*\%(\s*,\s*[A-Za-z_][A-Za-z0-9_]*\)*\s*>\)\?" contains=zapGenericDecl

syntax match zapIftypeTypeParam "\<iftype\>\s\+\zs[A-Za-z_][A-Za-z0-9_]*\ze\s*=="
syntax match zapIftypeMatchType "\<iftype\>\s\+[A-Za-z_][A-Za-z0-9_]*\s*==\s*\zs[A-Za-z_][A-Za-z0-9_]*\%(\s*\.\s*[A-Za-z_][A-Za-z0-9_]*\)*\%(\s*<\s*[A-Za-z_][A-Za-z0-9_]*\%(\s*,\s*[A-Za-z_][A-Za-z0-9_]*\)*\s*>\)\?" contains=zapGenericDecl

" Operators
syntax match zapLogicalOperator "&&\|||"
syntax match zapComparisonOperator "==\|!=\|>=\|<=\|>\|<\ze\%([^A-Za-z_]\|$\)"
syntax match zapAssignmentOperator "="
syntax match zapArithmeticOperator "[+\-/%^~]"
syntax match zapUnaryOperator "!"
syntax match zapPointerOperator "\%(^\|[^A-Za-z0-9_]\)\zs[&*]\ze\%([^=]\|$\)"

" Highlight links
highlight default link zapConditional Conditional
highlight default link zapRepeat Repeat
highlight default link zapStatement Statement
highlight default link zapStorageClass StorageClass
highlight default link zapStructure Structure

highlight default link zapLineComment Comment
highlight default link zapString String
highlight default link zapEscape SpecialChar
highlight default link zapChar Character
highlight default link zapNumber Number
highlight default link zapBoolean Boolean

highlight default link zapNamespace Include
highlight default link zapFunction Function
highlight default link zapFunHeader Function
highlight default link zapUnsafeFunHeader Function
highlight default link zapExtFunHeader Function
highlight default link zapFunctionGenericDecl Special
highlight default link zapFunctionCall Identifier

highlight default link zapTypeDecl Type
highlight default link zapVarDecl Identifier
highlight default link zapParameter Identifier
highlight default link zapType Type
highlight default link zapReturnType Type
highlight default link zapBuiltinType Type

highlight default link zapGenericDecl Special
highlight default link zapGenericTypeArg Type
highlight default link zapGenericComma Delimiter

highlight default link zapWhereTypeParam Type
highlight default link zapWhereBoundType Type
highlight default link zapIftypeTypeParam Type
highlight default link zapIftypeMatchType Type

highlight default link zapLogicalOperator Operator
highlight default link zapComparisonOperator Operator
highlight default link zapAssignmentOperator Operator
highlight default link zapArithmeticOperator Operator
highlight default link zapUnaryOperator Operator
highlight default link zapPointerOperator Operator

let b:current_syntax = "zap"
