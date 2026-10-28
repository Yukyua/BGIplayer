package com.bgiplayer.bgi_ui_demo

import android.app.Activity
import android.app.ActivityManager
import android.content.Intent
import android.net.Uri
import android.provider.DocumentsContract
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodChannel
import java.io.File
import java.util.concurrent.Executors

class MainActivity : FlutterActivity() {
    private var events: EventChannel.EventSink? = null
    private var logPath: String? = null
    private var removing = false
    private val runningGame get() = getSharedPreferences("engine",MODE_PRIVATE).getString("activePath",null)
    private var importing: MethodChannel.Result? = null
    private val worker = Executors.newSingleThreadExecutor()
    override fun configureFlutterEngine(engine: FlutterEngine) {
        super.configureFlutterEngine(engine)
        EventChannel(engine.dartExecutor.binaryMessenger,"bgiplayer/engine/events").setStreamHandler(object:EventChannel.StreamHandler {
            override fun onListen(args:Any?,sink:EventChannel.EventSink){ events=sink }
            override fun onCancel(args:Any?){events=null}
        })
        MethodChannel(engine.dartExecutor.binaryMessenger,"bgiplayer/engine").setMethodCallHandler { call,result ->
            when(call.method) {
                "launch" -> {
                    val path=call.argument<String>("path")
                    if(removing) result.error("busy","正在删除游戏副本",null)
                    else if(path==null || !(File(path,"sysprg.arc").isFile || File(path,"Archive/sysprg.arc").isFile)) {
                        result.error("directory","游戏目录缺少 sysprg.arc",null)
                    } else {
                        val intent=Intent(this,GameActivity::class.java)
                        for(key in listOf("path","title","renderer","defaultFont","logPath","iconPath")) intent.putExtra(key,call.argument<String>(key))
                        logPath=call.argument<String>("logPath")
                        getSharedPreferences("engine",MODE_PRIVATE).edit().putString("activePath",File(path).canonicalPath).apply()
                        startActivityForResult(intent,41);events?.success(mapOf("type" to "ready", "message" to logPath));result.success(null)
                    }
                }
                "stop" -> {sendBroadcast(Intent(GameActivity.STOP).setPackage(packageName));result.success(null)}
                "pause","resume" -> result.success(null) // SDLActivity receives Android lifecycle transitions.
                else -> result.notImplemented()
            }
        }
        MethodChannel(engine.dartExecutor.binaryMessenger,"bgiplayer/fonts").setMethodCallHandler { call,result ->
            if(call.method!="list")result.notImplemented()
            else result.success(File("/system/fonts").listFiles()?.filter{it.isFile && it.extension.lowercase() in setOf("ttf","otf","ttc") }?.sortedBy{it.name}?.map{mapOf("name" to it.nameWithoutExtension,"path" to it.absolutePath)}?:emptyList<Map<String,String>>())
        }
        MethodChannel(engine.dartExecutor.binaryMessenger,"bgiplayer/storage").setMethodCallHandler { call,result ->
            when(call.method) {
                "importGame" -> if(importing!=null || removing) result.error("busy","已有目录操作正在执行",null)
                    else { importing=result;startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION),42) }
                "removeGame" -> if(importing!=null || removing) result.error("busy","已有目录操作正在执行",null)
                    else {
                        val path=call.argument<String>("path")
                        val engineRunning=(getSystemService(ACTIVITY_SERVICE) as ActivityManager).runningAppProcesses?.any{it.processName=="$packageName:engine"}==true
                        val active=if(engineRunning)runningGame else null
                        removing=true
                        worker.execute {
                            try {
                                if(engineRunning && active==null)throw IllegalStateException("请先退出正在运行的游戏")
                                PrivateGameStorage.remove(filesDir,path,active);runOnUiThread{removing=false;result.success(null)} }
                            catch(e:Exception){runOnUiThread{removing=false;result.error("remove",e.message,null)}}
                        }
                    }
                else -> result.notImplemented()
            }
        }
    }
    @Deprecated("Activity result retained for the SDL host contract")
    override fun onActivityResult(request:Int,result:Int,data:Intent?) {
        super.onActivityResult(request,result,data)
        if(request==41){
            getSharedPreferences("engine",MODE_PRIVATE).edit().remove("activePath").apply()
            if(result==Activity.RESULT_FIRST_USER){
                val tail=try{logPath?.let{File(it).readLines().takeLast(24).joinToString("\n")}}catch(_:Exception){null}
                events?.success(mapOf("type" to "error","message" to "引擎退出（${data?.getIntExtra("exitCode",1)}）\n${tail?:""}"))
            }else events?.success(mapOf("type" to "exited"))
            return
        }
        if(request!=42)return
        val pending=importing?:return
        val tree=data?.data
        if(result!=Activity.RESULT_OK || tree==null){importing=null;pending.success(null);return}
        try{contentResolver.takePersistableUriPermission(tree,Intent.FLAG_GRANT_READ_URI_PERMISSION)}catch(_:SecurityException){/* Copy can still use this Activity grant. */}
        worker.execute {
            var staging:File?=null
            try {
                val root=DocumentsContract.buildDocumentUriUsingTree(tree,DocumentsContract.getTreeDocumentId(tree))
                val name=contentResolver.query(root,arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME),null,null,null)!!.use { c->c.moveToFirst();safe(c.getString(0)) }
                val games=File(filesDir,"Games").apply{mkdirs()};val destination=File(games,name)
                if(destination.exists())throw IllegalStateException("此游戏已导入，请先移除旧目录，防止覆盖存档")
                staging=File(cacheDir,"bgi-game-import").apply{if(exists())deleteRecursively();check(mkdirs())}
                copyTree(tree,DocumentsContract.getTreeDocumentId(tree),staging!!)
                if(!(File(staging,"sysprg.arc").isFile || File(staging,"Archive/sysprg.arc").isFile))throw IllegalStateException("所选目录缺少 sysprg.arc")
                if(!staging!!.renameTo(destination))throw IllegalStateException("无法完成导入")
                runOnUiThread{importing=null;pending.success(destination.absolutePath)}
            } catch(e:Exception) {staging?.deleteRecursively();runOnUiThread{importing=null;pending.error("import",e.message,null)}}
        }
    }
    private fun safe(name:String):String {require(name.isNotEmpty() && name!="." && name!=".." && !name.contains('/') && !name.contains('\\')){"无效文件名"};return name}
    private fun copyTree(tree:Uri,id:String,target:File) {
        val children=DocumentsContract.buildChildDocumentsUriUsingTree(tree,id)
        contentResolver.query(children,arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID,DocumentsContract.Document.COLUMN_DISPLAY_NAME,DocumentsContract.Document.COLUMN_MIME_TYPE),null,null,null)!!.use { c ->
            while(c.moveToNext()) {
                val childId=c.getString(0);val name=safe(c.getString(1));val destination=File(target,name)
                if(c.getString(2)==DocumentsContract.Document.MIME_TYPE_DIR){check(destination.mkdir());copyTree(tree,childId,destination)}
                else contentResolver.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree,childId))!!.use{input->destination.outputStream().use{output->input.copyTo(output,1024*1024)}}
            }
        }
    }
}
