static int model_http_probe(int argc,char** argv){
  if(argc!=3)return 2;auto start=std::chrono::steady_clock::now();
  auto reply=native_http(argv[2],"{}",{"Content-Type: application/json"},false,{}, {},false,true,3);
  auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
  std::cout<<json_dump(native_web::obj({{"status",native_web::number(reply.status)},{"error",native_web::str(reply.error)},{"elapsed",native_web::number(elapsed)}}))<<'\n';return 0;
}
