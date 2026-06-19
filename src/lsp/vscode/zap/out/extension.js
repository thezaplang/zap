"use strict";
var __awaiter = (this && this.__awaiter) || function (thisArg, _arguments, P, generator) {
    function adopt(value) { return value instanceof P ? value : new P(function (resolve) { resolve(value); }); }
    return new (P || (P = Promise))(function (resolve, reject) {
        function fulfilled(value) { try { step(generator.next(value)); } catch (e) { reject(e); } }
        function rejected(value) { try { step(generator["throw"](value)); } catch (e) { reject(e); } }
        function step(result) { result.done ? resolve(result.value) : adopt(result.value).then(fulfilled, rejected); }
        step((generator = generator.apply(thisArg, _arguments || [])).next());
    });
};
Object.defineProperty(exports, "__esModule", { value: true });
exports.activate = activate;
exports.deactivate = deactivate;
const fs = require("fs");
const path = require("path");
const child_process_1 = require("child_process");
const vscode_1 = require("vscode");
const node_1 = require("vscode-languageclient/node");
let client;
function isValidStdlibDir(candidate) {
    const prelude = path.join(candidate, "prelude.zp");
    return (fs.existsSync(candidate) &&
        fs.statSync(candidate).isDirectory() &&
        fs.existsSync(prelude) &&
        fs.statSync(prelude).isFile());
}
function isValidCoreDir(candidate) {
    const core = path.join(candidate, "core.zp");
    return (fs.existsSync(candidate) &&
        fs.statSync(candidate).isDirectory() &&
        fs.existsSync(core) &&
        fs.statSync(core).isFile());
}
function detectWorkspaceStdlibPath() {
    for (const folder of vscode_1.workspace.workspaceFolders || []) {
        const candidate = path.join(folder.uri.fsPath, "std");
        if (isValidStdlibDir(candidate)) {
            return fs.realpathSync(candidate);
        }
    }
    return "";
}
function detectWorkspaceCorePath() {
    for (const folder of vscode_1.workspace.workspaceFolders || []) {
        const candidate = path.join(folder.uri.fsPath, "core");
        if (isValidCoreDir(candidate)) {
            return fs.realpathSync(candidate);
        }
    }
    return "";
}
function isExecutableFile(candidate) {
    try {
        return fs.existsSync(candidate) && fs.statSync(candidate).isFile();
    }
    catch (_a) {
        return false;
    }
}
function detectWorkspaceZapcPath() {
    for (const folder of vscode_1.workspace.workspaceFolders || []) {
        const exeName = process.platform === "win32" ? "zapc.exe" : "zapc";
        const candidate = path.join(folder.uri.fsPath, "build", exeName);
        if (isExecutableFile(candidate)) {
            return candidate;
        }
    }
    return "";
}
function resolveZapcPath(context) {
    const config = vscode_1.workspace.getConfiguration("zap-lsp");
    const configuredZapcPath = (config.get("zapcPath") || "").trim();
    if (configuredZapcPath && isExecutableFile(configuredZapcPath)) {
        return configuredZapcPath;
    }
    const exeName = process.platform === "win32" ? "zapc.exe" : "zapc";
    const bundledZapcPath = context.asAbsolutePath(path.join("bin", exeName));
    if (isExecutableFile(bundledZapcPath)) {
        return bundledZapcPath;
    }
    return detectWorkspaceZapcPath();
}
function queryStdlibPathFromZapc(zapcPath) {
    if (!zapcPath) {
        return "";
    }
    try {
        const output = (0, child_process_1.execFileSync)(zapcPath, ["--print-stdlib-path"], {
            encoding: "utf8",
            stdio: ["ignore", "pipe", "ignore"],
        }).trim();
        if (output && isValidStdlibDir(output)) {
            return fs.realpathSync(output);
        }
    }
    catch (_a) {
        // ignore invalid compiler path or output and continue with fallbacks
    }
    return "";
}
function queryCorePathFromZapc(zapcPath) {
    if (!zapcPath) {
        return "";
    }
    try {
        const output = (0, child_process_1.execFileSync)(zapcPath, ["--print-core-path"], {
            encoding: "utf8",
            stdio: ["ignore", "pipe", "ignore"],
        }).trim();
        if (output && isValidCoreDir(output)) {
            return fs.realpathSync(output);
        }
    }
    catch (_a) {
        // ignore invalid compiler path or output and continue with fallbacks
    }
    return "";
}
function resolveStdlibPath(zapcPath) {
    const config = vscode_1.workspace.getConfiguration("zap-lsp");
    const configuredStdlibPath = (config.get("stdlibPath") || "").trim();
    if (configuredStdlibPath) {
        try {
            const resolved = fs.realpathSync(configuredStdlibPath);
            if (isValidStdlibDir(resolved)) {
                return resolved;
            }
        }
        catch (_a) {
            // ignore invalid configured path and continue with fallbacks
        }
    }
    const zapcStdlibPath = queryStdlibPathFromZapc(zapcPath);
    if (zapcStdlibPath) {
        return zapcStdlibPath;
    }
    return detectWorkspaceStdlibPath();
}
function resolveCorePath(zapcPath) {
    const config = vscode_1.workspace.getConfiguration("zap-lsp");
    const configuredCorePath = (config.get("corePath") || "").trim();
    if (configuredCorePath) {
        try {
            const resolved = fs.realpathSync(configuredCorePath);
            if (isValidCoreDir(resolved)) {
                return resolved;
            }
        }
        catch (_a) {
            // ignore invalid configured path and continue with fallbacks
        }
    }
    const zapcCorePath = queryCorePathFromZapc(zapcPath);
    if (zapcCorePath) {
        return zapcCorePath;
    }
    return detectWorkspaceCorePath();
}
function activate(context) {
    return __awaiter(this, void 0, void 0, function* () {
        const config = vscode_1.workspace.getConfiguration("zap-lsp");
        const configuredPath = (config.get("path") || "").trim();
        const bundledServerPath = context.asAbsolutePath(path.join("bin", "zap-lsp"));
        const lspPath = configuredPath || bundledServerPath;
        const zapcPath = resolveZapcPath(context);
        const corePath = resolveCorePath(zapcPath);
        const stdlibPath = resolveStdlibPath(zapcPath);
        const env = Object.assign({}, process.env);
        if (!configuredPath && fs.existsSync(bundledServerPath)) {
            fs.chmodSync(bundledServerPath, 0o755);
        }
        if (corePath) {
            env.ZAPC_CORE_DIR = corePath;
        }
        if (stdlibPath) {
            env.ZAPC_STDLIB_DIR = stdlibPath;
        }
        const outputChannel = vscode_1.window.createOutputChannel("Zap LSP");
        const serverOptions = {
            run: {
                command: lspPath,
                transport: node_1.TransportKind.stdio,
                options: { env },
            },
            debug: {
                command: lspPath,
                transport: node_1.TransportKind.stdio,
                options: { env },
            },
        };
        const clientOptions = {
            documentSelector: [{ scheme: "file", language: "zap" }],
            outputChannel,
        };
        client = new node_1.LanguageClient("zap-lsp", "Zap LSP", serverOptions, clientOptions);
        try {
            yield client.start();
        }
        catch (error) {
            outputChannel.appendLine(String(error));
            vscode_1.window.showErrorMessage(`Zap LSP failed to start. Check the "Zap LSP" output channel.`);
            throw error;
        }
    });
}
function deactivate() {
    return __awaiter(this, void 0, void 0, function* () {
        yield (client === null || client === void 0 ? void 0 : client.dispose());
        client = undefined;
    });
}
//# sourceMappingURL=extension.js.map