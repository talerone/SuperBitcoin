file(GLOB sources "*.cpp")

link_directories(../rpc)

add_executable(sbtc-tx ${sources} )
target_include_directories(sbtc-tx PUBLIC ${CMAKE_CURRENT_BINARY_DIR} )


IF (ENABLE_STATIC_FLAG)
    set(LIB_FILE -ldl libsnappy.a)
ELSE ()
    set(LIB_FILE )
ENDIF ()


target_link_libraries(sbtc-tx

        libboost_random.a ${Secp256k1_LIBRARY} ${Secp256k1_LIBRARY} contract-api eventmanager ${Secp256k1_LIBRARY} libboost_random.a contract libboost_random.a ${Secp256k1_LIBRARY} base
        compat config  libboost_random.a contract  libboost_random.a  sbtccore mempool chaincontrol utils  p2p  framework   ${Secp256k1_LIBRARY} contract-api ${Boost_LIBRARIES} contract ${Boost_LIBRARIES}  miner  rpc  univalue  wallet
        ${EVENT_LIBRARIES} ${LOG4CPP_LIBRARYS} libevent_pthreads.so ${Boost_LIBRARIES} miniupnpc ${OPENSSL_LIBRARIES}
        ${LIBDB_CXX_LIBRARIES} ${LEVELDB_LIBRARIES} libmemenv.a ${Secp256k1_LIBRARY}  ${LIB_FILE}
        )
