import * as fs from 'fs';
import * as path from 'path';
import { workspace, window, ExtensionContext } from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;

function detectStdlibPath(): string {
  for (const folder of workspace.workspaceFolders || []) {
    const candidate = path.join(folder.uri.fsPath, 'std');
    if (fs.existsSync(candidate)) {
      return candidate;
    }
  }
  return '';
}

export async function activate(context: ExtensionContext) {
  const config = workspace.getConfiguration('zap-lsp');
  const configuredPath = (config.get<string>('path') || '').trim();
  const configuredStdlibPath = (config.get<string>('stdlibPath') || '').trim();
  const bundledServerPath = context.asAbsolutePath(path.join('bin', 'zap-lsp'));
  const lspPath = configuredPath || bundledServerPath;
  const stdlibPath = configuredStdlibPath || detectStdlibPath();
  const env = { ...process.env };
  if (!configuredPath && fs.existsSync(bundledServerPath)) {
    fs.chmodSync(bundledServerPath, 0o755);
  }
  if (stdlibPath) {
    env.ZAPC_STDLIB_DIR = stdlibPath;
  }
  const outputChannel = window.createOutputChannel('Zap LSP');

  const serverOptions: ServerOptions = {
    run: {
      command: lspPath,
      transport: TransportKind.stdio,
      options: { env }
    },
    debug: {
      command: lspPath,
      transport: TransportKind.stdio,
      options: { env }
    }
  };

  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: 'file', language: 'zap' }],
    outputChannel,
  };

  client = new LanguageClient(
    'zap-lsp',
    'Zap LSP',
    serverOptions,
    clientOptions
  );

  try {
    await client.start();
  } catch (error) {
    outputChannel.appendLine(String(error));
    window.showErrorMessage(`Zap LSP failed to start. Check the "Zap LSP" output channel.`);
    throw error;
  }
}

export async function deactivate() {
  await client?.dispose();
  client = undefined;
}
