local M = {}

local function repo_root()
  return vim.fn.fnamemodify(debug.getinfo(1, "S").source:sub(2), ":p:h:h:h:h:h:h")
end

local function find_executable()
  local repo_server = repo_root() .. "/build/zap-lsp"
  if vim.fn.executable(repo_server) == 1 then
    return repo_server
  end

  local path_server = vim.fn.exepath("zap-lsp")
  if path_server ~= "" then
    return path_server
  end

  return "zap-lsp"
end

local function detect_stdlib(root_dir)
  local candidates = {}
  if root_dir and root_dir ~= "" then
    table.insert(candidates, root_dir .. "/std")
  end

  local repo_std = repo_root() .. "/std"
  table.insert(candidates, repo_std)

  for _, candidate in ipairs(candidates) do
    if vim.fn.isdirectory(candidate) == 1 then
      return candidate
    end
  end

  return nil
end

local function make_capabilities()
  local capabilities = vim.lsp.protocol.make_client_capabilities()
  local ok, cmp_nvim_lsp = pcall(require, "cmp_nvim_lsp")
  if ok then
    capabilities = cmp_nvim_lsp.default_capabilities(capabilities)
  end
  return capabilities
end

local function trim_empty_lines(lines)
  while #lines > 0 and lines[1] == "" do
    table.remove(lines, 1)
  end
  while #lines > 0 and lines[#lines] == "" do
    table.remove(lines, #lines)
  end
  return lines
end

local function on_attach(client, bufnr)
  local function open_float(method, params_builder)
    local params = params_builder()
    vim.lsp.buf_request(bufnr, method, params, function(err, result, ctx, _)
      if err or not result then
        return
      end

      if method == "textDocument/hover" then
        local markdown_lines = vim.lsp.util.convert_input_to_markdown_lines(result.contents)
        markdown_lines = trim_empty_lines(markdown_lines)
        if vim.tbl_isempty(markdown_lines) then
          return
        end
        vim.lsp.util.open_floating_preview(markdown_lines, "markdown", {
          border = "rounded",
          focusable = true,
          max_width = 80,
        })
        return
      end

      if method == "textDocument/signatureHelp" then
        if not result.signatures or vim.tbl_isempty(result.signatures) then
          return
        end
        local active_sig = (result.activeSignature or 0) + 1
        local sig = result.signatures[active_sig]
        if not sig or not sig.label then
          return
        end
        vim.lsp.util.open_floating_preview({ sig.label }, "zap", {
          border = "rounded",
          focusable = false,
          max_width = 80,
        })
      end
    end)
  end

  local function hover_float()
    open_float("textDocument/hover", function()
      return vim.lsp.util.make_position_params(0, client.offset_encoding)
    end)
  end

  local function signature_float()
    open_float("textDocument/signatureHelp", function()
      return vim.lsp.util.make_position_params(0, client.offset_encoding)
    end)
  end

  local opts = { buffer = bufnr, silent = true }
  vim.keymap.set("n", "gd", vim.lsp.buf.definition, opts)
  vim.keymap.set("n", "K", hover_float, opts)
  vim.keymap.set("n", "<C-k>", signature_float, opts)
  vim.keymap.set("i", "<C-k>", signature_float, opts)
  vim.keymap.set("n", "<leader>rn", vim.lsp.buf.rename, opts)
  vim.keymap.set("n", "<leader>ca", vim.lsp.buf.code_action, opts)
  vim.keymap.set("n", "gr", vim.lsp.buf.references, opts)
end

function M.make()
  local util = require("lspconfig.util")

  return {
    cmd = { find_executable() },
    cmd_env = {},
    filetypes = { "zap" },
    root_dir = util.root_pattern("zap.json", ".git"),
    single_file_support = true,
    capabilities = make_capabilities(),
    on_attach = on_attach,
    before_init = function(_, config)
      local stdlib = detect_stdlib(config.root_dir)
      if stdlib then
        config.cmd_env = vim.tbl_extend("force", config.cmd_env or {}, {
          ZAPC_STDLIB_DIR = stdlib,
        })
      end
    end,
  }
end

return M
