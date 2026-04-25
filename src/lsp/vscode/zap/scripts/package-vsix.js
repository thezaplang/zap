#!/usr/bin/env node

const fs = require("fs");
const path = require("path");
const ts = require("typescript");
const glob = require("glob");
const yazl = require("yazl");
const vsce = require("@vscode/vsce/out/package.js");

async function stageBundledServer(cwd) {
    const repoRoot = path.resolve(cwd, "..", "..", "..", "..");
    const serverSource = path.join(repoRoot, "build", "zap-lsp");
    const serverTarget = path.join(cwd, "bin", "zap-lsp");

    await fs.promises.access(serverSource, fs.constants.R_OK);

    await fs.promises.mkdir(path.dirname(serverTarget), { recursive: true });
    await fs.promises.copyFile(serverSource, serverTarget);
    await fs.promises.chmod(serverTarget, 0o755);
}

async function stageBundledStdlib(cwd) {
    const repoRoot = path.resolve(cwd, "..", "..", "..", "..");
    const stdlibSource = path.join(repoRoot, "std");
    const stdlibTarget = path.join(cwd, "stdlib");

    await fs.promises.access(stdlibSource, fs.constants.R_OK);
    await fs.promises.rm(stdlibTarget, { recursive: true, force: true });
    await fs.promises.cp(stdlibSource, stdlibTarget, { recursive: true });
}

function compileTypeScript(cwd) {
    const configPath = ts.findConfigFile(
        cwd,
        ts.sys.fileExists,
        "tsconfig.json",
    );
    if (!configPath) {
        throw new Error("tsconfig.json not found");
    }

    const readResult = ts.readConfigFile(configPath, ts.sys.readFile);
    if (readResult.error) {
        throw new Error(
            ts.flattenDiagnosticMessageText(readResult.error.messageText, "\n"),
        );
    }

    const parsed = ts.parseJsonConfigFileContent(
        readResult.config,
        ts.sys,
        path.dirname(configPath),
    );

    const program = ts.createProgram({
        rootNames: parsed.fileNames,
        options: parsed.options,
    });
    const emitResult = program.emit();
    const diagnostics = ts
        .getPreEmitDiagnostics(program)
        .concat(emitResult.diagnostics || []);

    if (diagnostics.length > 0) {
        const host = {
            getCanonicalFileName: (fileName) => fileName,
            getCurrentDirectory: () => cwd,
            getNewLine: () => "\n",
        };
        throw new Error(
            ts.formatDiagnosticsWithColorAndContext(diagnostics, host),
        );
    }
}

function collectManifestFiles(cwd, manifest) {
    const entries = new Set();
    for (const pattern of manifest.files || []) {
        for (const match of glob.sync(pattern, {
            cwd,
            nodir: true,
            dot: true,
        })) {
            entries.add(match.replace(/\\/g, "/"));
        }
    }
    return Array.from(entries).sort();
}

async function main() {
    const cwd = process.cwd();
    const manifest = await vsce.readManifest(cwd);

    compileTypeScript(cwd);
    await stageBundledServer(cwd);
    await stageBundledStdlib(cwd);

    const files = collectManifestFiles(cwd, manifest).map((file) => ({
        path: `extension/${file}`,
        localPath: path.join(cwd, file),
    }));

    const processors = vsce
        .createDefaultProcessors(manifest, {})
        .filter(
            (processor) =>
                processor.constructor.name !== "LaunchEntryPointProcessor",
        );
    const processedFiles = await vsce.processFiles(processors, files);

    const outPath = path.join(cwd, `${manifest.name}-${manifest.version}.vsix`);
    await fs.promises.rm(outPath, { force: true });

    await new Promise((resolve, reject) => {
        const zip = new yazl.ZipFile();
        for (const file of processedFiles) {
            if (Object.prototype.hasOwnProperty.call(file, "contents")) {
                const buffer = Buffer.isBuffer(file.contents)
                    ? file.contents
                    : Buffer.from(file.contents, "utf8");
                zip.addBuffer(buffer, file.path);
            } else {
                zip.addFile(file.localPath, file.path);
            }
        }

        zip.end();
        const output = fs.createWriteStream(outPath);
        zip.outputStream.pipe(output);
        zip.outputStream.on("error", reject);
        output.on("error", reject);
        output.on("close", resolve);
    });

    console.log(`Created ${outPath}`);
}

main().catch((error) => {
    console.error(error instanceof Error ? error.message : error);
    process.exit(1);
});
