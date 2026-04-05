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
const vscode_1 = require("vscode");
const node_1 = require("vscode-languageclient/node");
let client;
function detectStdlibPath() {
    for (const folder of vscode_1.workspace.workspaceFolders || []) {
        const candidate = path.join(folder.uri.fsPath, 'std');
        if (fs.existsSync(candidate)) {
            return candidate;
        }
    }
    return '';
}
function activate(context) {
    return __awaiter(this, void 0, void 0, function* () {
        const config = vscode_1.workspace.getConfiguration('zap-lsp');
        const configuredPath = (config.get('path') || '').trim();
        const configuredStdlibPath = (config.get('stdlibPath') || '').trim();
        const bundledServerPath = context.asAbsolutePath(path.join('bin', 'zap-lsp'));
        const lspPath = configuredPath || bundledServerPath;
        const stdlibPath = configuredStdlibPath || detectStdlibPath();
        const env = Object.assign({}, process.env);
        if (!configuredPath && fs.existsSync(bundledServerPath)) {
            fs.chmodSync(bundledServerPath, 0o755);
        }
        if (stdlibPath) {
            env.ZAPC_STDLIB_DIR = stdlibPath;
        }
        const outputChannel = vscode_1.window.createOutputChannel('Zap LSP');
        const serverOptions = {
            run: {
                command: lspPath,
                transport: node_1.TransportKind.stdio,
                options: { env }
            },
            debug: {
                command: lspPath,
                transport: node_1.TransportKind.stdio,
                options: { env }
            }
        };
        const clientOptions = {
            documentSelector: [{ scheme: 'file', language: 'zap' }],
            outputChannel,
        };
        client = new node_1.LanguageClient('zap-lsp', 'Zap LSP', serverOptions, clientOptions);
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