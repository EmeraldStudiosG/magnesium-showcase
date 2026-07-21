"use strict";
var __createBinding = (this && this.__createBinding) || (Object.create ? (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    var desc = Object.getOwnPropertyDescriptor(m, k);
    if (!desc || ("get" in desc ? !m.__esModule : desc.writable || desc.configurable)) {
      desc = { enumerable: true, get: function() { return m[k]; } };
    }
    Object.defineProperty(o, k2, desc);
}) : (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    o[k2] = m[k];
}));
var __setModuleDefault = (this && this.__setModuleDefault) || (Object.create ? (function(o, v) {
    Object.defineProperty(o, "default", { enumerable: true, value: v });
}) : function(o, v) {
    o["default"] = v;
});
var __importStar = (this && this.__importStar) || (function () {
    var ownKeys = function(o) {
        ownKeys = Object.getOwnPropertyNames || function (o) {
            var ar = [];
            for (var k in o) if (Object.prototype.hasOwnProperty.call(o, k)) ar[ar.length] = k;
            return ar;
        };
        return ownKeys(o);
    };
    return function (mod) {
        if (mod && mod.__esModule) return mod;
        var result = {};
        if (mod != null) for (var k = ownKeys(mod), i = 0; i < k.length; i++) if (k[i] !== "default") __createBinding(result, mod, k[i]);
        __setModuleDefault(result, mod);
        return result;
    };
})();
Object.defineProperty(exports, "__esModule", { value: true });
exports.activate = activate;
exports.deactivate = deactivate;
const vscode = __importStar(require("vscode"));
const node_1 = require("vscode-languageclient/node");
let client;
let statusItem;
let previousTheme;
const MG_THEME = 'Magnesium Dark';
function isMgDoc(document) {
    return !!document && document.languageId === 'magnesium';
}
function updateStatus(document) {
    if (!isMgDoc(document)) {
        statusItem.hide();
        return;
    }
    const diagnostics = vscode.languages.getDiagnostics(document.uri);
    const errors = diagnostics.filter(d => d.severity === vscode.DiagnosticSeverity.Error).length;
    if (errors > 0) {
        statusItem.text = '$(error) Magnesium';
        statusItem.backgroundColor = new vscode.ThemeColor('statusBarItem.errorBackground');
        statusItem.tooltip = `Magnesium LSP: ${errors} error(s)`;
    }
    else {
        statusItem.text = '$(check) Magnesium';
        statusItem.backgroundColor = undefined;
        statusItem.tooltip = 'Magnesium LSP active';
    }
    statusItem.show();
}
function switchTheme(toMg) {
    const config = vscode.workspace.getConfiguration('workbench');
    const current = config.get('colorTheme', '');
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
async function stopLanguageServer() {
    if (!client)
        return;
    const oldClient = client;
    client = undefined;
    await oldClient.stop();
}
async function startLanguageServer(context) {
    await stopLanguageServer();
    const config = vscode.workspace.getConfiguration('magnesium');
    const magnesiumPath = config.get('executablePath', 'magnesium');
    const serverOptions = {
        run: {
            command: magnesiumPath,
            args: ['--lsp'],
            transport: node_1.TransportKind.stdio,
        },
        debug: {
            command: magnesiumPath,
            args: ['--lsp'],
            transport: node_1.TransportKind.stdio,
        },
    };
    const clientOptions = {
        documentSelector: [{ scheme: 'file', language: 'magnesium' }],
        synchronize: {
            fileEvents: vscode.workspace.createFileSystemWatcher('**/*.mg'),
        },
    };
    client = new node_1.LanguageClient('magnesiumLanguageServer', 'Magnesium Language Server', serverOptions, clientOptions);
    context.subscriptions.push(client);
    await client.start();
    updateStatus(vscode.window.activeTextEditor?.document);
}
async function activate(context) {
    statusItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 0);
    statusItem.text = '$(sync~spin) Magnesium';
    statusItem.tooltip = 'Starting Magnesium LSP';
    context.subscriptions.push(statusItem);
    context.subscriptions.push(vscode.commands.registerCommand('magnesium.restartServer', async () => {
        statusItem.text = '$(sync~spin) Magnesium';
        statusItem.tooltip = 'Restarting Magnesium LSP';
        statusItem.show();
        await startLanguageServer(context);
    }));
    context.subscriptions.push(vscode.window.onDidChangeActiveTextEditor(editor => {
        const document = editor?.document;
        updateStatus(document);
        switchTheme(isMgDoc(document));
    }));
    context.subscriptions.push(vscode.languages.onDidChangeDiagnostics(event => {
        const active = vscode.window.activeTextEditor?.document;
        if (active && event.uris.some(uri => uri.toString() === active.uri.toString())) {
            updateStatus(active);
        }
    }));
    context.subscriptions.push(vscode.workspace.onDidChangeConfiguration(async (event) => {
        if (event.affectsConfiguration('magnesium.executablePath')) {
            await startLanguageServer(context);
        }
    }));
    await startLanguageServer(context);
    const active = vscode.window.activeTextEditor?.document;
    updateStatus(active);
    switchTheme(isMgDoc(active));
}
async function deactivate() {
    switchTheme(false);
    await stopLanguageServer();
}
//# sourceMappingURL=extension.js.map