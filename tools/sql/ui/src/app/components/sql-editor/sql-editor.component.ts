import { autocompletion, closeBrackets, closeBracketsKeymap, completionKeymap } from '@codemirror/autocomplete';
import { defaultKeymap, history, historyKeymap } from '@codemirror/commands';
import { SQLDialect, SQLite, sql } from '@codemirror/lang-sql';
import { bracketMatching, defaultHighlightStyle, syntaxHighlighting } from '@codemirror/language';
import { Compartment, EditorState, Extension } from '@codemirror/state';
import { EditorView, highlightActiveLine, keymap, lineNumbers, placeholder } from '@codemirror/view';
import { AfterViewInit, Component, ElementRef, OnDestroy, effect, input, model, output, viewChild } from '@angular/core';

import { Schema } from '../../models/api.models';

/** Table and view names with their columns, in the shape the SQL language support completes from. */
export function completionSchema(schema: Schema): Record<string, string[]> {
  const names: Record<string, string[]> = {};
  for (const relation of [...schema.tables, ...schema.views]) {
    names[relation.name] = relation.columns.map(column => column.name);
  }
  return names;
}

/** An SQL editor with syntax colors and completion of keywords, functions, tables and columns. */
@Component({
  selector: 'app-sql-editor',
  template: '<div class="host" #host></div>',
  styleUrl: './sql-editor.component.scss',
})
export class SqlEditorComponent implements AfterViewInit, OnDestroy {
  readonly value = model('');
  readonly schema = input<Schema>({ tables: [], views: [], indexes: [], triggers: [] });
  readonly functions = input<string[]>([]);
  /** Ctrl+Enter. */
  readonly run = output<void>();

  private readonly host = viewChild.required<ElementRef<HTMLElement>>('host');
  private readonly language = new Compartment();
  private view: EditorView | undefined;

  constructor() {
    effect(() => {
      const text = this.value();
      const view = this.view;
      if (view && text !== view.state.doc.toString()) {
        view.dispatch({ changes: { from: 0, to: view.state.doc.length, insert: text } });
      }
    });
    effect(() => {
      const extension = this.languageExtension(this.schema(), this.functions());
      this.view?.dispatch({ effects: this.language.reconfigure(extension) });
    });
  }

  ngAfterViewInit(): void {
    this.view = new EditorView({
      parent: this.host().nativeElement,
      state: EditorState.create({
        doc: this.value(),
        extensions: [
          lineNumbers(),
          highlightActiveLine(),
          history(),
          bracketMatching(),
          closeBrackets(),
          autocompletion(),
          syntaxHighlighting(defaultHighlightStyle),
          placeholder('Type SQL here'),
          this.language.of(this.languageExtension(this.schema(), this.functions())),
          keymap.of([
            {
              key: 'Mod-Enter',
              run: () => {
                this.run.emit();
                return true;
              },
            },
            ...closeBracketsKeymap,
            ...completionKeymap,
            ...historyKeymap,
            ...defaultKeymap,
          ]),
          EditorView.updateListener.of(update => {
            if (update.docChanged) {
              this.value.set(update.state.doc.toString());
            }
          }),
          EditorView.theme({
            '&': { height: '100%', fontSize: '13px', backgroundColor: 'var(--mat-sys-surface)' },
            '&.cm-focused': { outline: '2px solid var(--mat-sys-primary)', outlineOffset: '-1px' },
            '.cm-scroller': { fontFamily: "ui-monospace, SFMono-Regular, Menlo, Consolas, 'Liberation Mono', monospace", overflow: 'auto' },
            '.cm-gutters': { backgroundColor: 'var(--mat-sys-surface-container)', color: 'var(--mat-sys-outline)', border: 'none' },
          }),
        ],
      }),
    });
  }

  ngOnDestroy(): void {
    this.view?.destroy();
  }

  /** What is selected, or the whole text when nothing is. */
  selectedText(): string {
    const state = this.view?.state;
    if (!state) {
      return this.value();
    }
    const { from, to } = state.selection.main;
    return from === to ? state.doc.toString() : state.sliceDoc(from, to);
  }

  private languageExtension(schema: Schema, functions: string[]): Extension {
    // The list also holds operators such as ->, which are not words to complete.
    const names = functions.filter(name => /^\w+$/.test(name));
    const dialect = SQLDialect.define({ ...SQLite.spec, builtin: names.join(' ') });
    return sql({ dialect, schema: completionSchema(schema), upperCaseKeywords: true });
  }
}
