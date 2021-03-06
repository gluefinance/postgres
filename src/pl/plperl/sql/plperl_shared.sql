-- test plperl.on_plperl_init via the shared hash
-- (must be done before plperl is first used)

-- Avoid need for custom_variable_classes = 'plperl'
LOAD 'plperl';

-- testing on_plperl_init gets run, and that it can alter %_SHARED
SET plperl.on_plperl_init = '$_SHARED{on_init} = 42';

-- test the shared hash

create function setme(key text, val text) returns void language plperl as $$

  my $key = shift;
  my $val = shift;
  $_SHARED{$key}= $val;

$$;

create function getme(key text) returns text language plperl as $$

  my $key = shift;
  return $_SHARED{$key};

$$;

select setme('ourkey','ourval');

select getme('ourkey');

select getme('on_init');
