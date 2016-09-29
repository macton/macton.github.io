define(function (require) {

  var script_table = {
  };

  var scripts = {
    link: function( source_path, target_path ) {
      script_table[ source_path ] = { link: target_path };
    },
    append: function( source_path, line ) {
      if ( script_table[ source_path ] == null ) {
        script_table[ source_path ] = { content: [] };
      }
      script_table[ source_path ].content.push( line );
    },
    get: function( source_path ) {
      if ( script_table[ source_path ] == null ) {
        return null;
      }

      if ( script_table[ source_path ].link ) {
        return scripts.get( script_table[ source_path ].link );
      }

      return script_table[ source_path ].content;
    },
  };

  return scripts;
});
