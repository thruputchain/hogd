namespace hogd {

typedef opaque Bytes<>;

struct DogwoodStandaloneConfig {
  ValidatorConfig validator_config;
  RouterConfig router_config;
  DogwoodConfig dogwood_config;
};

struct DogwoodReceived {
  ValidatorID from;
  opaque payload<>;
};

program DogwoodAPI
{
  version DogwoodAPI1
  {

    void dogwoodapi_null(void) = 0;

    void dogwoodapi_send(Milliseconds, Bytes) = 10;

    DogwoodReceived dogwoodapi_receive(Milliseconds) = 20;
  }
  = 1;
}
= 0x8ee88ee8;

} // namespace hogd
