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
function resolveZapcPath() {
    const config = vscode_1.workspace.getConfiguration("zap-lsp");
    const configuredZapcPath = (config.get("zapcPath") || "").trim();
    if (configuredZapcPath && isExecutableFile(configuredZapcPath)) {
        return configuredZapcPath;
    }
    return detectWorkspaceZapcPath() || "zapc";
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
function configUriForFirstWorkspace() {
    var _a;
    const folder = (_a = vscode_1.workspace.workspaceFolders) === null || _a === void 0 ? void 0 : _a[0];
    return folder ? vscode_1.Uri.joinPath(folder.uri, "zaplsp.json") : undefined;
}
function configExists(configUri) {
    return __awaiter(this, void 0, void 0, function* () {
        if (!configUri) {
            return false;
        }
        try {
            const stat = yield vscode_1.workspace.fs.stat(configUri);
            return (stat.type & vscode_1.FileType.File) !== 0;
        }
        catch (_a) {
            return false;
        }
    });
}
function makeConfiguration(paths) {
    const coreParent = path.dirname(paths.corePath);
    const stdlibParent = path.dirname(paths.stdlibPath);
    if (coreParent === stdlibParent &&
        path.basename(paths.corePath) === "core" &&
        path.basename(paths.stdlibPath) === "std") {
        return {
            zapRoot: coreParent,
            corePath: "core",
            stdlibPath: "std",
        };
    }
    return {
        corePath: paths.corePath,
        stdlibPath: paths.stdlibPath,
    };
}
function writeConfiguration(configUri, paths) {
    return __awaiter(this, void 0, void 0, function* () {
        try {
            const contents = `${JSON.stringify(makeConfiguration(paths), null, 2)}\n`;
            yield vscode_1.workspace.fs.writeFile(configUri, Buffer.from(contents, "utf8"));
            vscode_1.window.showInformationMessage(`Created ${configUri.fsPath}`);
            return true;
        }
        catch (error) {
            vscode_1.window.showErrorMessage(`Could not create zaplsp.json: ${String(error)}`);
            return false;
        }
    });
}
function selectZapInstallation() {
    return __awaiter(this, void 0, void 0, function* () {
        const selected = yield vscode_1.window.showOpenDialog({
            canSelectFiles: false,
            canSelectFolders: true,
            canSelectMany: false,
            openLabel: "Select Zap Installation",
        });
        if (!(selected === null || selected === void 0 ? void 0 : selected[0])) {
            return undefined;
        }
        const root = selected[0].fsPath;
        const corePath = path.join(root, "core");
        const stdlibPath = path.join(root, "std");
        if (!isValidCoreDir(corePath) || !isValidStdlibDir(stdlibPath)) {
            vscode_1.window.showErrorMessage("The selected directory must contain core/core.zp and std/prelude.zp.");
            return undefined;
        }
        return {
            corePath: fs.realpathSync(corePath),
            stdlibPath: fs.realpathSync(stdlibPath),
        };
    });
}
function offerWorkspaceConfiguration(context, detectedPaths) {
    return __awaiter(this, void 0, void 0, function* () {
        const configUri = configUriForFirstWorkspace();
        if (!configUri) {
            return false;
        }
        if (yield configExists(configUri)) {
            return true;
        }
        if (!vscode_1.workspace.isTrusted) {
            return false;
        }
        const promptKey = `zaplsp.prompted:${configUri.toString()}`;
        if (context.workspaceState.get(promptKey)) {
            return false;
        }
        const createAction = detectedPaths.corePath && detectedPaths.stdlibPath
            ? "Create Configuration"
            : undefined;
        const selectAction = "Select Zap Installation";
        const choices = createAction
            ? [createAction, selectAction, "Not Now"]
            : [selectAction, "Not Now"];
        const choice = yield vscode_1.window.showInformationMessage("This Zap workspace has no zaplsp.json. Create one to configure core and stdlib paths?", ...choices);
        if (createAction && choice === createAction) {
            const created = yield writeConfiguration(configUri, detectedPaths);
            yield context.workspaceState.update(promptKey, created);
            return created;
        }
        if (choice === selectAction) {
            const selectedPaths = yield selectZapInstallation();
            if (!selectedPaths) {
                return false;
            }
            const created = yield writeConfiguration(configUri, selectedPaths);
            yield context.workspaceState.update(promptKey, created);
            return created;
        }
        yield context.workspaceState.update(promptKey, true);
        return false;
    });
}
function activate(context) {
    return __awaiter(this, void 0, void 0, function* () {
        const config = vscode_1.workspace.getConfiguration("zap-lsp");
        const configuredPath = (config.get("path") || "").trim();
        const bundledServerPath = context.asAbsolutePath(path.join("bin", "zap-lsp"));
        const lspPath = configuredPath || bundledServerPath;
        const zapcPath = resolveZapcPath();
        const corePath = resolveCorePath(zapcPath);
        const stdlibPath = resolveStdlibPath(zapcPath);
        const hasWorkspaceConfiguration = yield offerWorkspaceConfiguration(context, { corePath, stdlibPath });
        if (!configuredPath && fs.existsSync(bundledServerPath)) {
            fs.chmodSync(bundledServerPath, 0o755);
        }
        const outputChannel = vscode_1.window.createOutputChannel("Zap LSP");
        const serverOptions = {
            run: {
                command: lspPath,
                transport: node_1.TransportKind.stdio,
            },
            debug: {
                command: lspPath,
                transport: node_1.TransportKind.stdio,
            },
        };
        const configuredCorePath = (config.get("corePath") || "").trim();
        const configuredStdlibPath = (config.get("stdlibPath") || "").trim();
        const thorConfigWatcher = vscode_1.workspace.createFileSystemWatcher("**/thor.toml");
        const zapSourceWatcher = vscode_1.workspace.createFileSystemWatcher("**/*.zp");
        context.subscriptions.push(thorConfigWatcher, zapSourceWatcher);
        const clientOptions = {
            documentSelector: [{ scheme: "file", language: "zap" }],
            outputChannel,
            synchronize: {
                fileEvents: [thorConfigWatcher, zapSourceWatcher],
            },
            initializationOptions: {
                corePath: configuredCorePath ||
                    (!hasWorkspaceConfiguration ? corePath : undefined),
                stdlibPath: configuredStdlibPath ||
                    (!hasWorkspaceConfiguration ? stdlibPath : undefined),
            },
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