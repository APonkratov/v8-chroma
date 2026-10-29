export type ThemePreview = {id:string;name:string;dark:boolean;colors:Record<string,string>;editor:Record<string,string>};

const references = import.meta.glob<string>('../../../assets/screenshots/configurator-*.jpg', {eager:true,query:'?url',import:'default'});

export function stockReference(themeId:string):string {
  const name:Record<string,string> = {
    'github-dark-default':'github-dark-default','github-light-default':'github-light-default',
    'visual-studio-dark-cpp':'visual-studio-dark','visual-studio-light':'visual-studio-light',
  };
  return references[`../../../assets/screenshots/configurator-${name[themeId] ?? themeId}.jpg`];
}
