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
const vscode_1 = require("vscode");
const node_1 = require("vscode-languageclient/node");
let client;
function activate(context) {
    return __awaiter(this, void 0, void 0, function* () {
        const outputChannel = vscode_1.window.createOutputChannel("Zap LSP");
        const thorConfigWatcher = vscode_1.workspace.createFileSystemWatcher("**/thor.toml");
        const zapSourceWatcher = vscode_1.workspace.createFileSystemWatcher("**/*.zp");
        context.subscriptions.push(thorConfigWatcher, zapSourceWatcher, outputChannel);
        const serverOptions = {
            run: { command: "zap-lsp", transport: node_1.TransportKind.stdio },
            debug: { command: "zap-lsp", transport: node_1.TransportKind.stdio },
        };
        const clientOptions = {
            documentSelector: [{ scheme: "file", language: "zap" }],
            outputChannel,
            synchronize: { fileEvents: [thorConfigWatcher, zapSourceWatcher] },
        };
        client = new node_1.LanguageClient("zap-lsp", "Zap LSP", serverOptions, clientOptions);
        try {
            yield client.start();
        }
        catch (error) {
            outputChannel.appendLine(String(error));
            vscode_1.window.showErrorMessage('Zap LSP failed to start. Install Zap with zapup and make sure "zap-lsp" is on PATH.');
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