import { currentCompletions, startCompletion } from '@codemirror/autocomplete';
import { syntaxTree } from '@codemirror/language';
import { EditorView } from '@codemirror/view';
import { provideZonelessChangeDetection } from '@angular/core';
import { ComponentFixture, TestBed } from '@angular/core/testing';

import { sampleSchema, settle } from '../../testing/test-helpers';
import { SqlEditorComponent, completionSchema } from './sql-editor.component';

describe('completionSchema', () => {
  it('lists the columns of the tables and the views', () => {
    expect(completionSchema(sampleSchema())).toEqual({
      person: ['id', 'name', 'age', 'note'],
      tag: ['label'],
      adults: ['id', 'name'],
    });
  });
});

describe('SqlEditorComponent', () => {
  let fixture: ComponentFixture<SqlEditorComponent>;
  let view: EditorView;

  beforeEach(async () => {
    TestBed.configureTestingModule({ providers: [provideZonelessChangeDetection()] });
    fixture = TestBed.createComponent(SqlEditorComponent);
    fixture.componentRef.setInput('schema', sampleSchema());
    fixture.componentRef.setInput('functions', ['count', 'lower', 'json_extract', '->']);
    await fixture.whenStable();
    view = EditorView.findFromDOM(fixture.nativeElement.querySelector('.cm-editor'))!;
  });

  async function type(text: string): Promise<void> {
    view.dispatch({ changes: { from: 0, to: view.state.doc.length, insert: text }, selection: { anchor: text.length } });
    await settle();
  }

  /** What the editor offers after `text`, waiting for the completion to arrive. */
  async function offered(text: string): Promise<string[]> {
    await type(text);
    startCompletion(view);
    for (let attempt = 0; attempt < 100; attempt++) {
      const labels = currentCompletions(view.state).map(completion => completion.label);
      if (labels.length > 0) {
        return labels;
      }
      await new Promise(resolve => setTimeout(resolve, 10));
    }
    return [];
  }

  it('starts empty and says what to do', () => {
    expect(view.state.doc.toString()).toBe('');
    expect(fixture.nativeElement.querySelector('.cm-placeholder').textContent).toBe('Type SQL here');
  });

  it('shows the text it is given, also when that changes', async () => {
    fixture.componentRef.setInput('value', 'SELECT 1');
    await settle();
    expect(view.state.doc.toString()).toBe('SELECT 1');
    fixture.componentRef.setInput('value', 'SELECT 2');
    await settle();
    expect(view.state.doc.toString()).toBe('SELECT 2');
  });

  it('tells its owner what is typed', async () => {
    await type('SELECT name FROM person');
    expect(fixture.componentInstance.value()).toBe('SELECT name FROM person');
  });

  it('reads the selection, or all the text when nothing is selected', async () => {
    await type('SELECT 1; SELECT 2;');
    expect(fixture.componentInstance.selectedText()).toBe('SELECT 1; SELECT 2;');
    view.dispatch({ selection: { anchor: 10, head: 19 } });
    expect(fixture.componentInstance.selectedText()).toBe('SELECT 2;');
  });

  it('runs on Ctrl+Enter', async () => {
    let runs = 0;
    fixture.componentInstance.run.subscribe(() => runs++);
    view.contentDOM.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', ctrlKey: true, bubbles: true, cancelable: true }));
    expect(runs).toBe(1);
  });

  describe('syntax colors', () => {
    const kinds = (text: string) => {
      const found = new Set<string>();
      syntaxTree(EditorView.findFromDOM(fixture.nativeElement.querySelector('.cm-editor'))!.state).iterate({
        enter: node => {
          found.add(node.name);
        },
      });
      return found;
    };

    it('tells keywords, strings, numbers and comments apart', async () => {
      await type("SELECT 'a', 12 FROM person -- note\n");
      const names = kinds('');
      for (const expected of ['Keyword', 'String', 'Number', 'LineComment']) {
        expect(names.has(expected)).withContext(expected).toBeTrue();
      }
    });

    it('colors the text', async () => {
      await type("SELECT 'a' FROM person");
      const colored = Array.from(fixture.nativeElement.querySelectorAll('.cm-line span')).filter(span => (span as HTMLElement).className !== '');
      expect(colored.length).toBeGreaterThanOrEqual(2);
    });

    it('leaves operators in the function list out of the words', async () => {
      await type('SELECT a -> b');
      expect(kinds('').has('Builtin')).toBeFalse();
    });

    it('knows the functions the server lists as built-ins', async () => {
      await type('SELECT json_extract(note, 1)');
      expect(kinds('').has('Builtin')).toBeTrue();
    });
  });

  describe('completion', () => {
    it('offers keywords in capitals', async () => {
      expect(await offered('SEL')).toContain('SELECT');
    });

    it('offers the tables and the views after FROM', async () => {
      const labels = await offered('SELECT * FROM ');
      expect(labels).toEqual(jasmine.arrayContaining(['person', 'tag', 'adults']));
    });

    it('offers the columns of a table after its name and a dot', async () => {
      expect(await offered('SELECT person.')).toEqual(jasmine.arrayContaining(['id', 'name', 'age', 'note']));
    });

    it('offers the functions of the server, in capitals like the keywords', async () => {
      expect(await offered('SELECT low')).toContain('LOWER');
    });

    it('follows a change of the schema', async () => {
      fixture.componentRef.setInput('schema', { ...sampleSchema(), tables: [{ ...sampleSchema().tables[0], name: 'zebra' }] });
      await settle();
      expect(await offered('SELECT * FROM ze')).toContain('zebra');
    });
  });
});
