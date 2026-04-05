if vim.g.loaded_zap_nvim == 1 then
  return
end
vim.g.loaded_zap_nvim = 1

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
  end,
})

vim.api.nvim_create_user_command("ZapNvimInfo", function()
  local lines = {
    "runtime loaded: yes",
    "current filetype: " .. vim.bo.filetype,
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
