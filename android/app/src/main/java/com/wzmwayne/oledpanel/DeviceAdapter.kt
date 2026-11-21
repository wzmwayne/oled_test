package com.wzmwayne.oledpanel

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.card.MaterialCardView

class DeviceAdapter(
    private val onClick: (DeviceUi) -> Unit
) : RecyclerView.Adapter<DeviceAdapter.VH>() {

    data class DeviceUi(val name: String, val address: String, val rssi: Int)

    private val items = ArrayList<DeviceUi>()
    var connectedAddress: String? = null
    var connectingAddress: String? = null

    fun submit(newItems: List<DeviceUi>) {
        items.clear()
        items.addAll(newItems)
        notifyDataSetChanged()
    }

    fun updateConnectionState(connected: String?, connecting: String?) {
        connectedAddress = connected
        connectingAddress = connecting
        notifyDataSetChanged()
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH {
        val view = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_device, parent, false)
        return VH(view)
    }

    override fun getItemCount(): Int = items.size

    override fun onBindViewHolder(holder: VH, position: Int) {
        holder.bind(items[position])
    }

    inner class VH(itemView: View) : RecyclerView.ViewHolder(itemView) {
        private val card: MaterialCardView = itemView as MaterialCardView
        private val tvName: TextView = itemView.findViewById(R.id.tvDevName)
        private val tvMac: TextView = itemView.findViewById(R.id.tvDevMac)
        private val tvRssi: TextView = itemView.findViewById(R.id.tvDevRssi)
        private val tvState: TextView = itemView.findViewById(R.id.tvDevState)

        fun bind(item: DeviceUi) {
            val context = itemView.context
            tvName.text = item.name.ifBlank { "未命名设备" }
            tvMac.text = if (item.name.isBlank()) {
                item.address + "  · 广播未带名称"
            } else {
                item.address
            }
            tvRssi.text = Rssi.label(item.rssi)
            tvRssi.setTextColor(Rssi.color(context, item.rssi))

            when {
                item.address == connectedAddress -> {
                    tvState.visibility = View.VISIBLE
                    tvState.text = "已连接"
                    tvState.setTextColor(androidx.core.content.ContextCompat.getColor(context, R.color.ok))
                    card.setStrokeColor(androidx.core.content.ContextCompat.getColor(context, R.color.ok))
                }
                item.address == connectingAddress -> {
                    tvState.visibility = View.VISIBLE
                    tvState.text = "连接中…"
                    tvState.setTextColor(androidx.core.content.ContextCompat.getColor(context, R.color.warn))
                    card.setStrokeColor(androidx.core.content.ContextCompat.getColor(context, R.color.warn))
                }
                else -> {
                    tvState.visibility = View.GONE
                    card.setStrokeColor(androidx.core.content.ContextCompat.getColor(context, R.color.line))
                }
            }
            itemView.setOnClickListener { onClick(item) }
        }
    }
}
