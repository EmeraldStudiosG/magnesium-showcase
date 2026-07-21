import * as vscode from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind,
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;
let statusItem: vscode.StatusBarItem;
let previousTheme: string | undefined;

const MG_THEME = 'Magnesium Dark';

function isMgDoc(document: vscode.TextDocument | undefined): boolean {
    return !!document && document.languageId === 'magnesium';
}

function updateStatus(document: vscode.TextDocument | undefined) {
    if (!isMgDoc(document)) {
        statusItem.hide();
        return;
    }

    const diagnostics = vscode.languages.getDiagnostics(document!.uri);
    const errors = diagnostics.filter(d => d.severity === vscode.DiagnosticSeverity.Error).length;

    if (errors > 0) {
        statusItem.text = '$(error) Magnesium';
        statusItem.backgroundColor = new vscode.ThemeColor('statusBarItem.errorBackground');
        statusItem.tooltip = `Magnesium LSP: ${errors} error(s)`;
    } else {
        statusItem.text = '$(check) Magnesium';
        statusItem.backgroundColor = undefined;
        statusItem.tooltip = 'Magnesium LSP active';
    }
    statusItem.show();
}

function switchTheme(toMg: boolean) {
    const config = vscode.workspace.getConfiguration('workbench');
    const current = config.get<string>('colorTheme', '');

    if (toMg) {
        if (current !== MG_THEME && previousTheme === undefined) {
            previousTheme = current;
            void config.update('colorTheme', MG_THEME, vscode.ConfigurationTarget.Global);
        }
        return;
    }

    if (previousTheme !== undefined) {
        void config.update('colorTheme', previousTheme, vscode.ConfigurationTarget.Global);
        previousTheme = undefined;
    }
}

async function stopLanguageServer(): Promise<void> {
    if (!client) return;
    const oldClient = client;
    client = undefined;
    await oldClient.stop();
}

async function startLanguageServer(context: vscode.ExtensionContext): Promise<void> {
    await stopLanguageServer();

    const config = vscode.workspace.getConfiguration('magnesium');
    const magnesiumPath = config.get<string>('executablePath', 'magnesium');

    const serverOptions: ServerOptions = {
        run: {
            command: magnesiumPath,
            args: ['--lsp'],
            transport: TransportKind.stdio,
        },
        debug: {
            command: magnesiumPath,
            args: ['--lsp'],
            transport: TransportKind.stdio,
        },
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: 'file', language: 'magnesium' }],
        synchronize: {
            fileEvents: vscode.workspace.createFileSystemWatcher('**/*.mg'),
        },
    };

    client = new LanguageClient(
        'magnesiumLanguageServer',
        'Magnesium Language Server',
        serverOptions,
        clientOptions,
    );

    context.subscriptions.push(client);
    await client.start();
    updateStatus(vscode.window.activeTextEditor?.document);
}

export async function activate(context: vscode.ExtensionContext) {
    statusItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 0);
    statusItem.text = '$(sync~spin) Magnesium';
    statusItem.tooltip = 'Starting Magnesium LSP';
    context.subscriptions.push(statusItem);

    context.subscriptions.push(
        vscode.commands.registerCommand('magnesium.restartServer', async () => {
            statusItem.text = '$(sync~spin) Magnesium';
            statusItem.tooltip = 'Restarting Magnesium LSP';
            statusItem.show();
            await startLanguageServer(context);
        }),
    );

    context.subscriptions.push(
        vscode.window.onDidChangeActiveTextEditor(editor => {
            const document = editor?.document;
            updateStatus(document);
            switchTheme(isMgDoc(document));
        }),
    );

    context.subscriptions.push(
        vscode.languages.onDidChangeDiagnostics(event => {
            const active = vscode.window.activeTextEditor?.document;
            if (active && event.uris.some(uri => uri.toString() === active.uri.toString())) {
                updateStatus(active);
            }
        }),
    );

    context.subscriptions.push(
        vscode.workspace.onDidChangeConfiguration(async event => {
            if (event.affectsConfiguration('magnesium.executablePath')) {
                await startLanguageServer(context);
            }
        }),
    );

    await startLanguageServer(context);

    const active = vscode.window.activeTextEditor?.document;
    updateStatus(active);
    switchTheme(isMgDoc(active));
}

export async function deactivate(): Promise<void> {
    switchTheme(false);
    await stopLanguageServer();
}
