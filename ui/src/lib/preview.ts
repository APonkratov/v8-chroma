export type ThemePreview = {id:string;name:string;dark:boolean;colors:Record<string,string>;editor:Record<string,string>};

const references = import.meta.glob<string>('../../../assets/screenshots/configurator-*.jpg', {eager:true,query:'?url',import:'default'});

export function stockReference({id, dark}: Pick<ThemePreview, 'id' | 'dark'>):string {
  const name:Record<string,string> = {
    'visual-studio-dark-cpp':'visual-studio-dark','visual-studio-light':'visual-studio-light',
  };
  const image = (key: string) => references[`../../../assets/screenshots/configurator-${key}.jpg`];
  return image(name[id] ?? id) ?? image(dark ? 'github-dark-default' : 'github-light-default');
}
