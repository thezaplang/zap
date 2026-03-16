import * as path from 'path';
import { workspace, ExtensionContext } from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;

export async function activate(context: ExtensionContext) {
  const config = workspace.getConfiguration('zap-lsp');
  const lspPath = config.get<string>('path') || 'zap-lsp';

  const serverOptions: ServerOptions = {
    run: { command: lspPath, transport: TransportKind.stdio },
    debug: { command: lspPath, transport: TransportKind.stdio }
  };

  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: 'file', language: 'zap' }],
  };

  client = new LanguageClient(
    'zap-lsp',
    'Zap LSP',
    serverOptions,
    clientOptions
  );

  await client.start()
}

export async function deactivate() {
  await client?.dispose();
  client = undefined;
}