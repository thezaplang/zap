if vim.g.loaded_zap_nvim == 1 then
  return
end
vim.g.loaded_zap_nvim = 1

vim.lsp.handlers["textDocument/hover"] = vim.lsp.with(vim.lsp.handlers.hover, {
  border = "rounded",
  focusable = true,
  max_width = 80,
})

vim.lsp.handlers["textDocument/signatureHelp"] = vim.lsp.with(
  vim.lsp.handlers.signature_help,
  {
    border = "rounded",
    focusable = false,
    max_width = 80,
  }
)

local function apply_highlight_overrides()
  local function hi(group, opts)
    if vim.api.nvim_get_hl then
      vim.api.nvim_set_hl(0, group, opts)
    else
      local fg = opts.fg and (" guifg=" .. opts.fg) or ""
      local bg = opts.bg and (" guibg=" .. opts.bg) or ""
      local gui = opts.italic and " gui=italic" or (opts.bold and " gui=bold" or "")
      vim.cmd("highlight " .. group .. fg .. bg .. gui)
    end
  end

  -- Keep function names vivid.
  hi("zapFunction", { fg = "#82AAFF" })

  -- Make generic brackets/segment visibly different.
  hi("zapGenericDecl", { fg = "#C792EA", italic = true })

  -- Type parameters should not look like function names.
  hi("zapGenericTypeParam", { fg = "#FFCB6B", italic = true })
  hi("zapGenericTypeArg", { fg = "#FFCB6B", italic = true })

  -- Also differentiate where/iftype generic params.
  hi("zapWhereTypeParam", { fg = "#FFCB6B", italic = true })
  hi("zapIftypeTypeParam", { fg = "#FFCB6B", italic = true })

  -- Keep concrete matched/bound types in a separate tone.
  hi("zapWhereBoundType", { fg = "#89DDFF" })
  hi("zapIftypeMatchType", { fg = "#89DDFF" })
end

local function setup_lsp()
  if vim.g.loaded_zap_nvim_lsp == 1 then
    return
  end

  local ok, config = pcall(require, "zap_nvim.config")
  if not ok then
    return
  end

  if vim.lsp and type(vim.lsp.config) == "function" and type(vim.lsp.enable) == "function" then
    pcall(vim.lsp.config, "zap_lsp", config.make())
    pcall(vim.lsp.enable, "zap_lsp")
    vim.g.loaded_zap_nvim_lsp = 1
    return
  end

  local ok_lspconfig, lspconfig = pcall(require, "lspconfig")
  if not ok_lspconfig then
    return
  end

  local ok_configs, configs = pcall(require, "lspconfig.configs")
  if not ok_configs then
    return
  end

  if not configs.zap_lsp then
    configs.zap_lsp = {
      default_config = config.make(),
    }
  end

  lspconfig.zap_lsp.setup({})
  vim.g.loaded_zap_nvim_lsp = 1
end

if vim.filetype and type(vim.filetype.add) == "function" then
  vim.filetype.add({
    extension = {
      zp = "zap",
    },
  })
else
  vim.cmd([[
    augroup zap_filetype_lua
      autocmd!
      autocmd BufRead,BufNewFile *.zp setfiletype zap
    augroup END
  ]])
end

setup_lsp()

vim.api.nvim_create_autocmd("FileType", {
  pattern = "zap",
  callback = function(args)
    vim.bo[args.buf].commentstring = "// %s"
    vim.bo[args.buf].omnifunc = "v:lua.vim.lsp.omnifunc"

    -- Force reliable syntax activation for zap buffers.
    -- Some setups keep filetype=zap but never attach syntax groups.
    vim.bo[args.buf].syntax = "zap"

    -- Explicitly source Zap syntax for this buffer.
    pcall(vim.api.nvim_buf_call, args.buf, function()
      vim.cmd("silent! runtime! syntax/zap.vim")
      vim.cmd("silent! syntax sync fromstart")
    end)

    apply_highlight_overrides()
  end,
})

vim.api.nvim_create_user_command("ZapNvimInfo", function()
  local syn_name = vim.fn.synIDattr(vim.fn.synID(vim.fn.line("."), vim.fn.col("."), 1), "name")
  local syn_trans = vim.fn.synIDattr(vim.fn.synIDtrans(vim.fn.synID(vim.fn.line("."), vim.fn.col("."), 1)), "name")
  local syn_file = vim.fn.globpath(vim.o.runtimepath, "syntax/zap.vim", 1, 1)
  local syntax_source = "(not found in runtimepath)"
  if type(syn_file) == "table" and #syn_file > 0 then
    syntax_source = syn_file[1]
  elseif type(syn_file) == "string" and syn_file ~= "" then
    syntax_source = syn_file
  end

  local lines = {
    "runtime loaded: yes",
    "current filetype: " .. vim.bo.filetype,
    "current syntax: " .. (vim.bo.syntax ~= "" and vim.bo.syntax or "(empty)"),
    "syntax source: " .. syntax_source,
    "highlight under cursor: " .. (syn_name ~= "" and syn_name or "(none)"),
    "translated highlight: " .. (syn_trans ~= "" and syn_trans or "(none)"),
    "generic color override: enabled",
  }

  local ok, config = pcall(require, "zap_nvim.config")
  if ok then
    local lsp = config.make()
    table.insert(lines, "lsp cmd: " .. table.concat(lsp.cmd or {}, " "))
  else
    table.insert(lines, "lsp config load failed")
  end

  vim.notify(table.concat(lines, "\n"), vim.log.levels.INFO, { title = "zap.nvim" })
end, {})

vim.api.nvim_create_user_command("ZapNvimFixSyntax", function()
  local buf = vim.api.nvim_get_current_buf()

  vim.bo[buf].filetype = "zap"
  vim.bo[buf].syntax = "zap"

  pcall(vim.api.nvim_buf_call, buf, function()
    vim.cmd("silent! syntax enable")
    vim.cmd("silent! runtime! syntax/zap.vim")
    vim.cmd("silent! syntax sync fromstart")
    vim.cmd("silent! doautocmd <nomodeline> FileType zap")
  end)

  apply_highlight_overrides()

  local syn_name = vim.fn.synIDattr(vim.fn.synID(vim.fn.line("."), vim.fn.col("."), 1), "name")
  local syn_trans = vim.fn.synIDattr(vim.fn.synIDtrans(vim.fn.synID(vim.fn.line("."), vim.fn.col("."), 1)), "name")
  local msg = {
    "Zap syntax reattached for current buffer.",
    "highlight under cursor: " .. (syn_name ~= "" and syn_name or "(none)"),
    "translated highlight: " .. (syn_trans ~= "" and syn_trans or "(none)"),
  }

  vim.notify(table.concat(msg, "\n"), vim.log.levels.INFO, { title = "zap.nvim" })
end, {
  desc = "Reattach Zap syntax groups for current buffer",
})
