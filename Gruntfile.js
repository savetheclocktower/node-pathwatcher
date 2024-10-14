const DEFAULT_COMMAND_OPTIONS = {
  stdout: true,
  stderr: true,
  failOnError: true
};

function defineTasks (grunt) {
  grunt.initConfig({
    pkg: grunt.file.readJSON('package.json'),

    shell: {
      rebuild: {
        command: `npm build .`,
        options: DEFAULT_COMMAND_OPTIONS
      },

      test: {
        command: `npm test`,
        options: DEFAULT_COMMAND_OPTIONS
      },

      'update-atomdoc': {
        command: 'npm update grunt-atomdoc',
        options: DEFAULT_COMMAND_OPTIONS
      }
    }
  });

  grunt.loadNpmTasks('grunt-shell');
  grunt.loadNpmTasks('grunt-atomdoc');

  grunt.registerTask('default', ['shell:rebuild']);
  grunt.registerTask('test', ['default', 'shell:test']);

  // TODO: AtomDoc is not being generated now that we've decaffeinated the
  // source files. We should use `joanna` instead, but it needs some
  // modernization to understand current JS syntax.
  grunt.registerTask('prepublish', ['shell:update-atomdoc', 'atomdoc']);

  grunt.registerTask('clean', () => {
    let rm = require('rimraf').sync;
    rm('build');
    rm('lib');
    rm('api.json');
  });
}

module.exports = defineTasks;
