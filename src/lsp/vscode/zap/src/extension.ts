import * as fs from "fs";
import * as path from "path";
import { execFileSync } from "child_process";
import {
    workspace,
    window,
    ExtensionContext,
    FileType,
    Uri,
} from "vscode";
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

function resolveZapcPath(): string {
    const config = workspace.getConfiguration("zap-lsp");
    const configuredZapcPath = (config.get<string>("zapcPath") || "").trim();
    if (configuredZapcPath && isExecutableFile(configuredZapcPath)) {
        return configuredZapcPath;
    }

    return detectWorkspaceZapcPath() || "zapc";
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

interface RuntimePaths {
    corePath: string;
    stdlibPath: string;
}

function configUriForFirstWorkspace(): Uri | undefined {
    const folder = workspace.workspaceFolders?.[0];
    return folder ? Uri.joinPath(folder.uri, "zaplsp.json") : undefined;
}

async function configExists(configUri: Uri | undefined): Promise<boolean> {
    if (!configUri) {
        return false;
    }
    try {
        const stat = await workspace.fs.stat(configUri);
        return (stat.type & FileType.File) !== 0;
    } catch {
        return false;
    }
}

function makeConfiguration(paths: RuntimePaths): object {
    const coreParent = path.dirname(paths.corePath);
    const stdlibParent = path.dirname(paths.stdlibPath);
    if (
        coreParent === stdlibParent &&
        path.basename(paths.corePath) === "core" &&
        path.basename(paths.stdlibPath) === "std"
    ) {
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

async function writeConfiguration(
    configUri: Uri,
    paths: RuntimePaths,
): Promise<boolean> {
    try {
        const contents = `${JSON.stringify(makeConfiguration(paths), null, 2)}\n`;
        await workspace.fs.writeFile(configUri, Buffer.from(contents, "utf8"));
        window.showInformationMessage(`Created ${configUri.fsPath}`);
        return true;
    } catch (error) {
        window.showErrorMessage(
            `Could not create zaplsp.json: ${String(error)}`,
        );
        return false;
    }
}

async function selectZapInstallation(): Promise<RuntimePaths | undefined> {
    const selected = await window.showOpenDialog({
        canSelectFiles: false,
        canSelectFolders: true,
        canSelectMany: false,
        openLabel: "Select Zap Installation",
    });
    if (!selected?.[0]) {
        return undefined;
    }

    const root = selected[0].fsPath;
    const corePath = path.join(root, "core");
    const stdlibPath = path.join(root, "std");
    if (!isValidCoreDir(corePath) || !isValidStdlibDir(stdlibPath)) {
        window.showErrorMessage(
            "The selected directory must contain core/core.zp and std/prelude.zp.",
        );
        return undefined;
    }
    return {
        corePath: fs.realpathSync(corePath),
        stdlibPath: fs.realpathSync(stdlibPath),
    };
}

async function offerWorkspaceConfiguration(
    context: ExtensionContext,
    detectedPaths: RuntimePaths,
): Promise<boolean> {
    const configUri = configUriForFirstWorkspace();
    if (!configUri) {
        return false;
    }
    if (await configExists(configUri)) {
        return true;
    }
    if (!workspace.isTrusted) {
        return false;
    }

    const promptKey = `zaplsp.prompted:${configUri.toString()}`;
    if (context.workspaceState.get<boolean>(promptKey)) {
        return false;
    }

    const createAction =
        detectedPaths.corePath && detectedPaths.stdlibPath
            ? "Create Configuration"
            : undefined;
    const selectAction = "Select Zap Installation";
    const choices = createAction
        ? [createAction, selectAction, "Not Now"]
        : [selectAction, "Not Now"];
    const choice = await window.showInformationMessage(
        "This Zap workspace has no zaplsp.json. Create one to configure core and stdlib paths?",
        ...choices,
    );

    if (createAction && choice === createAction) {
        const created = await writeConfiguration(configUri, detectedPaths);
        await context.workspaceState.update(promptKey, created);
        return created;
    }
    if (choice === selectAction) {
        const selectedPaths = await selectZapInstallation();
        if (!selectedPaths) {
            return false;
        }
        const created = await writeConfiguration(configUri, selectedPaths);
        await context.workspaceState.update(promptKey, created);
        return created;
    }
    await context.workspaceState.update(promptKey, true);
    return false;
}

export async function activate(context: ExtensionContext) {
    const config = workspace.getConfiguration("zap-lsp");
    const configuredPath = (config.get<string>("path") || "").trim();
    const bundledServerPath = context.asAbsolutePath(
        path.join("bin", "zap-lsp"),
    );
    const lspPath = configuredPath || bundledServerPath;
    const zapcPath = resolveZapcPath();
    const corePath = resolveCorePath(zapcPath);
    const stdlibPath = resolveStdlibPath(zapcPath);
    const hasWorkspaceConfiguration = await offerWorkspaceConfiguration(
        context,
        { corePath, stdlibPath },
    );

    if (!configuredPath && fs.existsSync(bundledServerPath)) {
        fs.chmodSync(bundledServerPath, 0o755);
    }

    const outputChannel = window.createOutputChannel("Zap LSP");

    const serverOptions: ServerOptions = {
        run: {
            command: lspPath,
            transport: TransportKind.stdio,
        },
        debug: {
            command: lspPath,
            transport: TransportKind.stdio,
        },
    };

    const configuredCorePath = (
        config.get<string>("corePath") || ""
    ).trim();
    const configuredStdlibPath = (
        config.get<string>("stdlibPath") || ""
    ).trim();
    const thorConfigWatcher = workspace.createFileSystemWatcher("**/thor.toml");
    const zapSourceWatcher = workspace.createFileSystemWatcher("**/*.zp");
    context.subscriptions.push(thorConfigWatcher, zapSourceWatcher);
    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: "file", language: "zap" }],
        outputChannel,
        synchronize: {
            fileEvents: [thorConfigWatcher, zapSourceWatcher],
        },
        initializationOptions: {
            corePath:
                configuredCorePath ||
                (!hasWorkspaceConfiguration ? corePath : undefined),
            stdlibPath:
                configuredStdlibPath ||
                (!hasWorkspaceConfiguration ? stdlibPath : undefined),
        },
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
