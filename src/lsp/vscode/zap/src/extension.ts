import * as fs from "fs";
import * as path from "path";
import { execFileSync } from "child_process";
import { workspace, window, ExtensionContext } from "vscode";
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind,
} from "vscode-languageclient/node";

let client: LanguageClient | undefined;

function isValidStdlibDir(candidate: string): boolean {
    const prelude = path.join(candidate, "prelude.zp");
    return (
        fs.existsSync(candidate) &&
        fs.statSync(candidate).isDirectory() &&
        fs.existsSync(prelude) &&
        fs.statSync(prelude).isFile()
    );
}

function isValidCoreDir(candidate: string): boolean {
    const core = path.join(candidate, "core.zp");
    return (
        fs.existsSync(candidate) &&
        fs.statSync(candidate).isDirectory() &&
        fs.existsSync(core) &&
        fs.statSync(core).isFile()
    );
}

function detectWorkspaceStdlibPath(): string {
    for (const folder of workspace.workspaceFolders || []) {
        const candidate = path.join(folder.uri.fsPath, "std");
        if (isValidStdlibDir(candidate)) {
            return fs.realpathSync(candidate);
        }
    }
    return "";
}

function detectWorkspaceCorePath(): string {
    for (const folder of workspace.workspaceFolders || []) {
        const candidate = path.join(folder.uri.fsPath, "core");
        if (isValidCoreDir(candidate)) {
            return fs.realpathSync(candidate);
        }
    }
    return "";
}

function isExecutableFile(candidate: string): boolean {
    try {
        return fs.existsSync(candidate) && fs.statSync(candidate).isFile();
    } catch {
        return false;
    }
}

function detectWorkspaceZapcPath(): string {
    for (const folder of workspace.workspaceFolders || []) {
        const exeName = process.platform === "win32" ? "zapc.exe" : "zapc";
        const candidate = path.join(folder.uri.fsPath, "build", exeName);
        if (isExecutableFile(candidate)) {
            return candidate;
        }
    }
    return "";
}

function resolveZapcPath(context: ExtensionContext): string {
    const config = workspace.getConfiguration("zap-lsp");
    const configuredZapcPath = (config.get<string>("zapcPath") || "").trim();
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

function queryStdlibPathFromZapc(zapcPath: string): string {
    if (!zapcPath) {
        return "";
    }
    try {
        const output = execFileSync(zapcPath, ["--print-stdlib-path"], {
            encoding: "utf8",
            stdio: ["ignore", "pipe", "ignore"],
        }).trim();
        if (output && isValidStdlibDir(output)) {
            return fs.realpathSync(output);
        }
    } catch {
        // ignore invalid compiler path or output and continue with fallbacks
    }
    return "";
}

function queryCorePathFromZapc(zapcPath: string): string {
    if (!zapcPath) {
        return "";
    }
    try {
        const output = execFileSync(zapcPath, ["--print-core-path"], {
            encoding: "utf8",
            stdio: ["ignore", "pipe", "ignore"],
        }).trim();
        if (output && isValidCoreDir(output)) {
            return fs.realpathSync(output);
        }
    } catch {
        // ignore invalid compiler path or output and continue with fallbacks
    }
    return "";
}

function resolveStdlibPath(zapcPath: string): string {
    const config = workspace.getConfiguration("zap-lsp");
    const configuredStdlibPath = (
        config.get<string>("stdlibPath") || ""
    ).trim();
    if (configuredStdlibPath) {
        try {
            const resolved = fs.realpathSync(configuredStdlibPath);
            if (isValidStdlibDir(resolved)) {
                return resolved;
            }
        } catch {
            // ignore invalid configured path and continue with fallbacks
        }
    }

    const zapcStdlibPath = queryStdlibPathFromZapc(zapcPath);
    if (zapcStdlibPath) {
        return zapcStdlibPath;
    }

    return detectWorkspaceStdlibPath();
}

function resolveCorePath(zapcPath: string): string {
    const config = workspace.getConfiguration("zap-lsp");
    const configuredCorePath = (config.get<string>("corePath") || "").trim();
    if (configuredCorePath) {
        try {
            const resolved = fs.realpathSync(configuredCorePath);
            if (isValidCoreDir(resolved)) {
                return resolved;
            }
        } catch {
            // ignore invalid configured path and continue with fallbacks
        }
    }

    const zapcCorePath = queryCorePathFromZapc(zapcPath);
    if (zapcCorePath) {
        return zapcCorePath;
    }

    return detectWorkspaceCorePath();
}

export async function activate(context: ExtensionContext) {
    const config = workspace.getConfiguration("zap-lsp");
    const configuredPath = (config.get<string>("path") || "").trim();
    const bundledServerPath = context.asAbsolutePath(
        path.join("bin", "zap-lsp"),
    );
    const lspPath = configuredPath || bundledServerPath;
    const zapcPath = resolveZapcPath(context);
    const corePath = resolveCorePath(zapcPath);
    const stdlibPath = resolveStdlibPath(zapcPath);
    const env = { ...process.env };

    if (!configuredPath && fs.existsSync(bundledServerPath)) {
        fs.chmodSync(bundledServerPath, 0o755);
    }

    if (corePath) {
        env.ZAPC_CORE_DIR = corePath;
    }

    if (stdlibPath) {
        env.ZAPC_STDLIB_DIR = stdlibPath;
    }

    const outputChannel = window.createOutputChannel("Zap LSP");

    const serverOptions: ServerOptions = {
        run: {
            command: lspPath,
            transport: TransportKind.stdio,
            options: { env },
        },
        debug: {
            command: lspPath,
            transport: TransportKind.stdio,
            options: { env },
        },
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: "file", language: "zap" }],
        outputChannel,
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
            `Zap LSP failed to start. Check the "Zap LSP" output channel.`,
        );
        throw error;
    }
}

export async function deactivate() {
    await client?.dispose();
    client = undefined;
}
