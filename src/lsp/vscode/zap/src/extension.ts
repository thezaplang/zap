import { ExtensionContext, window, workspace } from "vscode";
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind,
} from "vscode-languageclient/node";

let client: LanguageClient | undefined;

export async function activate(context: ExtensionContext) {
    const outputChannel = window.createOutputChannel("Zap LSP");
    const thorConfigWatcher = workspace.createFileSystemWatcher("**/thor.toml");
    const zapSourceWatcher = workspace.createFileSystemWatcher("**/*.zp");
    context.subscriptions.push(thorConfigWatcher, zapSourceWatcher, outputChannel);

    const serverOptions: ServerOptions = {
        run: { command: "zap-lsp", transport: TransportKind.stdio },
        debug: { command: "zap-lsp", transport: TransportKind.stdio },
    };
    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: "file", language: "zap" }],
        outputChannel,
        synchronize: { fileEvents: [thorConfigWatcher, zapSourceWatcher] },
    };

    client = new LanguageClient(
        "zap-lsp",
        "Zap LSP",
        serverOptions,
        clientOptions,
    );

    try {
        await client.start();
    } catch (error) {
        outputChannel.appendLine(String(error));
        window.showErrorMessage(
            'Zap LSP failed to start. Install Zap with zapup and make sure "zap-lsp" is on PATH.',
        );
        throw error;
    }
}

export async function deactivate() {
    await client?.dispose();
    client = undefined;
}
