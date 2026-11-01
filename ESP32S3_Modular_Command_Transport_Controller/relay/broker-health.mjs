// Operator diagnostics deliberately use fixed messages, never raw errors or URLs.
export function brokerFailure(error){
  const code=String(error?.code??'');
  if(['4','5','134','135'].includes(code))return {phase:'auth-error',message:'Broker sign-in was rejected. Check the relay MQTT credentials.'};
  if(['CERT_HAS_EXPIRED','DEPTH_ZERO_SELF_SIGNED_CERT','SELF_SIGNED_CERT_IN_CHAIN','UNABLE_TO_VERIFY_LEAF_SIGNATURE','UNABLE_TO_GET_ISSUER_CERT_LOCALLY','ERR_TLS_CERT_ALTNAME_INVALID','EPROTO'].includes(code))return {phase:'tls-error',message:'Broker TLS verification or handshake failed. Check the certificate, CA, hostname and host clock.'};
  if(['ENOTFOUND','EAI_AGAIN'].includes(code))return {phase:'dns-error',message:'The broker hostname could not be resolved.'};
  return {phase:'connection-error',message:'The relay cannot reach the broker. Check its listener, network and firewall.'};
}
